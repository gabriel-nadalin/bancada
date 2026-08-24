#!/usr/bin/env python3
"""burstbench - burst/delay benchmark for SIP audio paths.

Drives any tool that exchanges raw s16le/8kHz/mono audio, using sine
bursts:

  gen      create a TX wav with the burst pattern
  analyze  compare TX and RX wavs, report per-burst delay + stats
  run      orchestrate a full benchmark with baresip, pjsua, or rtp_bridge

Examples
  # Run a full benchmark with any tool:
  burstbench.py run baresip [--peer sip:...]
  burstbench.py run pjsua   [--peer sip:...]
  burstbench.py run bridge  [--peer sip:...]

  # Or manually (same thing):
  burstbench.py gen audios/bench_tx.wav
  ./baresip_play -p sip:... -d audios/bench_tx.wav -D audios/rx.wav
  burstbench.py analyze audios/bench_tx.wav audios/rx.wav
"""

import argparse
import math
import os
import shlex
import subprocess
import sys
import time
import wave

HERE = os.path.dirname(os.path.abspath(__file__))
AUDIOS = os.path.join(HERE, "audios")
TX_WAV = os.path.join(AUDIOS, "bench_tx.wav")

SRATE = 8000
WINDOW_MS = 10                      # Goertzel window
AMP = 0.5 * 32767


# ---------------------------------------------------------------- pattern

def pattern_sample(idx, freq, burst_ms, period_ms):
    """sample value at global sample index (sine burst or silence)"""
    period = SRATE * period_ms // 1000
    burst = SRATE * burst_ms // 1000
    if idx % period < burst:
        return int(AMP * math.sin(2.0 * math.pi * freq * idx / SRATE))
    return 0


def read_wav(path):
    w = wave.open(path, "rb")
    try:
        if (w.getnchannels(), w.getsampwidth(), w.getframerate()) != (1, 2, SRATE):
            raise SystemExit(
                f"{path}: need mono s16 {SRATE} Hz wav, got "
                f"ch={w.getnchannels()} sw={w.getsampwidth()} "
                f"rate={w.getframerate()}")
        return w.readframes(w.getnframes())
    finally:
        w.close()



def detect_bursts_cadence(pcm_bytes, freq, cadence=(10, 40)):
    """Detect bursts using cadence-matched cross-correlation.

    Computes the Goertzel envelope, then applies a matched filter
    ``[+1/ons, -1/offs]`` over one burst period.  This yields
    ``mean(on_power) - mean(off_power)``, which cancels any constant
    baseline (e.g. ringback harmonic leakage at the target frequency)
    and measures only the burst energy *above* the local average.

    Returns list of burst onset times (milliseconds).
    """
    powers = goertzel_envelope(pcm_bytes, freq)
    ons, offs = cadence
    period = ons + offs
    if len(powers) < period:
        return []

    # Pad with zeros so the trailing off-windows of the last burst don't
    # fall past the end of the envelope.  Without this, a burst whose
    # start window is within `period` of the file end is invisible.
    powers = list(powers) + [0.0] * (period - 1)

    coef_on = 1.0 / ons
    coef_off = -1.0 / offs

    # Running-sum cross-correlation: O(N) with O(1) per window.
    on_sum = sum(powers[:ons])
    off_sum = sum(powers[ons:period])
    scores = [on_sum * coef_on + off_sum * coef_off]
    for i in range(1, len(powers) - period + 1):
        on_sum += powers[i + ons - 1] - powers[i - 1]
        off_sum += powers[i + period - 1] - powers[i + ons - 1]
        scores.append(on_sum * coef_on + off_sum * coef_off)

    max_score = max(scores)
    if max_score <= 0:
        return []

    thresh = max_score * 0.25

    # Peak-pick: find local maxima above threshold, skip one period ahead
    # after each hit (the cadence is fixed, so this avoids redundant
    # descending-slope peaks from the same burst).
    peaks = []
    i = 0
    while i < len(scores):
        if scores[i] > thresh:
            peak = i
            while peak + 1 < len(scores) and scores[peak + 1] > scores[peak]:
                peak += 1
            peaks.append(peak)
            i = peak + period
        else:
            i += 1

    return [w * WINDOW_MS for w in peaks]


def summary(delays):
    n = len(delays)
    if not n:
        return "no bursts paired"
    mean = sum(delays) / n
    var = sum((d - mean) ** 2 for d in delays) / n
    return (f"n={n} mean={mean:.1f} ms min={min(delays):.1f} ms "
            f"max={max(delays):.1f} ms stdev={math.sqrt(var):.1f} ms")


def goertzel_envelope(pcm_bytes, freq):
    """Goertzel power for each 10 ms window (the detection envelope)."""
    N = SRATE * WINDOW_MS // 1000
    wsz = N * 2
    coeff = 2.0 * math.cos(2.0 * math.pi * freq / SRATE)
    powers = []
    for off in range(0, len(pcm_bytes) - wsz + 1, wsz):
        s1 = s2 = 0.0
        for x in memoryview(pcm_bytes)[off:off + wsz].cast("h"):
            s0 = x + coeff * s1 - s2
            s2 = s1
            s1 = s0
        powers.append(max(0.0, s2 * s2 + s1 * s1 - coeff * s1 * s2))
    return powers


def estimate_delay(tx_env, rx_env, max_ms=500):
    """Pipeline delay (ms) via cross-correlation of Goertzel envelopes.

    Positive result = RX is delayed relative to TX.  The search is
    limited to ±``max_ms`` — this is the *pipeline* delay, not the
    absolute offset of the first burst in the recording.
    """
    n = min(len(tx_env), len(rx_env))
    max_k = max_ms // WINDOW_MS
    best_corr = -1.0
    best_k = 0
    for k in range(-max_k, max_k + 1):
        if k >= 0:
            length = n - k
            if length < 50:
                continue
            corr = sum(tx_env[i] * rx_env[i + k] for i in range(length))
        else:
            length = n + k
            if length < 50:
                continue
            corr = sum(tx_env[-k + i] * rx_env[i] for i in range(length))
        if corr > best_corr:
            best_corr = corr
            best_k = k
    return best_k * WINDOW_MS


# ---------------------------------------------------------------- commands

def cmd_gen(a):
    n = SRATE * a.dur
    buf = bytearray(n * 2)
    mv = memoryview(buf).cast("h")
    for i in range(n):
        mv[i] = pattern_sample(i, a.freq, a.burst, a.period)
    w = wave.open(a.tx_wav, "wb")
    w.setnchannels(1)
    w.setsampwidth(2)
    w.setframerate(SRATE)
    w.writeframes(bytes(buf))
    w.close()
    n_bursts = a.dur * 1000 // a.period
    print(f"{a.tx_wav}: {a.dur} s, {n_bursts} bursts "
          f"({a.freq} Hz, {a.burst} ms every {a.period} ms), "
          f"first at 0 ms", file=sys.stderr)


def cmd_analyze(a):
    tx_pcm = read_wav(a.tx_wav)
    rx_pcm = read_wav(a.rx_wav)

    tx = detect_bursts_cadence(tx_pcm, a.freq)
    rx = detect_bursts_cadence(rx_pcm, a.freq)
    print(f"tx bursts: {len(tx)}  rx bursts: {len(rx)}"
          "  (cadence cross-correlation)")

    # Pipeline delay via cross-correlation of Goertzel envelopes.
    tx_env = goertzel_envelope(tx_pcm, a.freq)
    rx_env = goertzel_envelope(rx_pcm, a.freq)
    pipeline_delay = estimate_delay(tx_env, rx_env, max(500, a.period * 2))
    print(f"pipeline delay: {pipeline_delay} ms"
          "  (cross-correlation of Goertzel envelope)")

    # Per-burst: pair each TX burst with the closest RX burst at
    # expected position (tx + pipeline_delay ± margin).
    margin = a.period // 3  # ~167 ms — generous to catch all
    missed = 0
    rows = []
    used = [False] * len(rx)
    for t in tx:
        expected = t + pipeline_delay
        match = None
        for j, r in enumerate(rx):
            if used[j]:
                continue
            if abs(r - expected) <= margin:
                if match is None or abs(r - expected) < abs(match[1] - expected):
                    match = (j, r)
        if match is not None:
            j, r = match
            used[j] = True
            delay = r - t
            rows.append((t, r, delay))
        else:
            rows.append((t, None, None))
            missed += 1

    # Print per-burst table
    n_cols = 3 + 3 * (len(rows) > 15)
    for i, (t, r, delay) in enumerate(rows):
        r_str = f"{r} ms" if r is not None else "---    "
        d_str = f"delay={delay} ms" if delay is not None else "MISSING"
        print(f"burst {i:3d}: tx={t:>7} ms  rx={r_str:>8}  {d_str}")

    found = len(tx) - missed
    print(f"\nresult: pipeline delay = {pipeline_delay} ms"
          f"  (of {len(tx)} bursts: {found} received, {missed} missing)")

    if missed > len(tx) // 4:
        print("warning: >25% of bursts missing", file=sys.stderr)

    if a.csv:
        with open(a.csv, "w") as f:
            f.write("burst,tx_ms,rx_ms,delay_ms\n")
            for i, (t, r, d) in enumerate(rows):
                f.write(f"{i},{t},{r if r is not None else ''},"
                       f"{d if d is not None else ''}\n")
        print(f"wrote {a.csv}")
    if missed:
        sys.exit(2)


# ---------------------------------------------------------------- run (orchestration)

def _ensure_tx_wav(freq, burst, period, dur=15):
    if not os.path.exists(TX_WAV):
        print(f"generating {TX_WAV} ...", file=sys.stderr)
        ns = argparse.Namespace(
            tx_wav=TX_WAV, dur=dur, freq=freq,
            burst=burst, period=period)
        cmd_gen(ns)


def _run_common(sp):
    sp.add_argument("--tx-wav", default=TX_WAV)
    sp.add_argument("--rx-wav", default=None)
    sp.add_argument("--peer", default=None)


def _run_analyze(a, rx):
    ns = argparse.Namespace(
        tx_wav=a.tx_wav, rx_wav=rx, freq=a.freq,
        burst=a.burst, period=a.period, csv=None)
    cmd_analyze(ns)


def cmd_run_baresip(a):
    _ensure_tx_wav(a.freq, a.burst, a.period, a.dur)
    rx = a.rx_wav or os.path.join(AUDIOS, "baresip_rx.wav")
    cmd = [os.path.join(HERE, "baresip_play"),
           "-p", a.peer, "-d", a.tx_wav, "-D", rx]
    print(f"+ {shlex.join(cmd)}", file=sys.stderr)
    subprocess.check_call(cmd)
    _run_analyze(a, rx)


def cmd_run_pjsua(a):
    _ensure_tx_wav(a.freq, a.burst, a.period, a.dur)
    rx = a.rx_wav or os.path.join(AUDIOS, "pjsua_rx.wav")
    cmd = [
        "pjsua",
        "--id", "sip:12@10.42.0.1",
        "--null-audio",
        "--play-file", a.tx_wav,
        "--auto-play",
        "--auto-play-hangup",
        "--rec-file", rx,
        "--auto-rec",
        "--no-vad",
        "--jb-max-size=0",
        "--capture-lat=5",
        "--playback-lat=5",
        "--quality=1",
        "--ptime", str(a.ptime),
        "--dis-codec", "PCMU",
        "--clock-rate", "8000",
        "--ec-tail", "0",
        a.peer,
    ]
    if a.auto_answer:
        cmd += ["--auto-answer", "200"]
    print(f"+ {shlex.join(cmd)}", file=sys.stderr)
    proc = subprocess.Popen(cmd, stdin=subprocess.PIPE)
    # pjsua hangs up after play-file finishes (--auto-play-hangup)
    # but doesn't exit — send 'q' to quit after a safety margin.
    import threading, time
    def _quit():
        time.sleep(a.dur + 5)
        try:
            proc.stdin.write(b"q\n")
            proc.stdin.flush()
        except BrokenPipeError:
            pass
    threading.Thread(target=_quit, daemon=True).start()
    proc.wait()
    _run_analyze(a, rx)


def cmd_run_bridge(a):
    _ensure_tx_wav(a.freq, a.burst, a.period, a.dur)
    rx = a.rx_wav or os.path.join(AUDIOS, "bridge_rx.wav")
    cmd = [os.path.join(HERE, "rtp_bridge"),
           "-p", a.peer, "-i", a.tx_wav, "-o", rx,
           "-t", str(a.ptime)]
    print(f"+ {shlex.join(cmd)}", file=sys.stderr)
    subprocess.check_call(cmd)
    _run_analyze(a, rx)


def cmd_run_slmodem(a):
    """Orchestrate slmodem_bridge + rtp_bridge for modem-over-RTP."""
    import threading

    slm_path = os.path.join(HERE, "slmodem_bridge")
    rtp_path = os.path.join(HERE, "rtp_bridge")

    if not os.path.exists(slm_path):
        print(f"error: {slm_path} not built; run 'make slmodem_bridge'",
              file=sys.stderr)
        sys.exit(1)

    rtp_cmd = [rtp_path, "-p", a.peer, "-t", str(a.ptime)]
    slm_cmd = [slm_path, "-m", a.slmodem_mode, "-M", str(a.modulation)]
    if getattr(a, "record", None):
        slm_cmd += ["-r", a.record]

    print(f"+ {shlex.join(rtp_cmd)}", file=sys.stderr)
    print(f"+ {shlex.join(slm_cmd)}", file=sys.stderr)

    rtp = subprocess.Popen(
        rtp_cmd,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        bufsize=0,
    )
    slm = subprocess.Popen(
        slm_cmd,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        bufsize=0,
    )

    def pump(src, dst, name):
        try:
            while True:
                chunk = src.read(8192)
                if not chunk:
                    break
                dst.write(chunk)
                dst.flush()
        except Exception as e:
            print(f"pump {name}: {e}", file=sys.stderr)

    def echo(src, prefix):
        try:
            for line in src:
                text = line.decode("utf-8", errors="replace").rstrip()
                print(f"[{prefix}] {text}", file=sys.stderr)
        except Exception:
            pass

    t1 = threading.Thread(target=pump, args=(slm.stdout, rtp.stdin, "slm->rtp"),
                          daemon=True)
    t2 = threading.Thread(target=pump, args=(rtp.stdout, slm.stdin, "rtp->slm"),
                          daemon=True)
    t3 = threading.Thread(target=echo, args=(slm.stderr, "slm"), daemon=True)
    t4 = threading.Thread(target=echo, args=(rtp.stderr, "rtp"), daemon=True)
    t1.start()
    t2.start()
    t3.start()
    t4.start()

    try:
        while rtp.poll() is None and slm.poll() is None:
            time.sleep(0.5)
    except KeyboardInterrupt:
        pass
    finally:
        for p in (rtp, slm):
            try:
                p.terminate()
                p.wait(timeout=3)
            except subprocess.TimeoutExpired:
                p.kill()


def main():
    p = argparse.ArgumentParser(
        prog="burstbench", description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="mode", required=True)

    def common(sp):
        sp.add_argument("--freq", type=int, default=1000,
                        help="burst sine frequency (default 1000 Hz)")
        sp.add_argument("--burst", type=int, default=100,
                        help="burst length in ms (default 100)")
        sp.add_argument("--period", type=int, default=500,
                        help="burst period in ms (default 500)")
        sp.add_argument("--dur", type=int, default=10,
                        help="duration in seconds (default 10)")
        sp.add_argument("--ptime", type=int, default=10,
                        help="frame size in ms (default 10)")

    g = sub.add_parser("gen", help="create TX burst wav")
    g.add_argument("tx_wav")
    common(g)
    g.set_defaults(fn=cmd_gen)

    an = sub.add_parser("analyze", help="pair TX/RX wavs, report delays")
    an.add_argument("tx_wav")
    an.add_argument("rx_wav")
    an.add_argument("--csv")
    common(an)
    an.set_defaults(fn=cmd_analyze)


    # run (orchestration)
    ru = sub.add_parser("run", help="run a full benchmark with a tool")
    rsub = ru.add_subparsers(dest="tool", required=True)

    bp = rsub.add_parser("baresip", help="run baresip_play benchmark")
    _run_common(bp)
    common(bp)
    bp.set_defaults(fn=cmd_run_baresip, peer="sip:11@10.42.0.102:5062;transport=udp")

    pp = rsub.add_parser("pjsua", help="run pjsua benchmark")
    _run_common(pp)
    pp.add_argument("--auto-answer", action="store_true",
                    help="auto-answer incoming calls")
    common(pp)
    pp.set_defaults(fn=cmd_run_pjsua, peer="sip:11@10.42.0.102:5062;transport=udp")

    bpr = rsub.add_parser("bridge", help="run rtp_bridge benchmark")
    _run_common(bpr)
    common(bpr)
    bpr.set_defaults(fn=cmd_run_bridge, peer="sip:11@10.42.0.102:5062;transport=udp")

    sp = rsub.add_parser("slmodem", help="run slmodem_bridge + rtp_bridge")
    _run_common(sp)
    sp.add_argument("--slmodem-mode", default="orig",
                    choices=["orig", "ans"],
                    help="slmodem mode: orig (default) or ans")
    sp.add_argument("--record", help="record slmodem RX audio to WAV")
    sp.add_argument("-M", "--modulation", type=int, default=122,
                    help="SREG_DP value (22=V.22 1200bps, 122=V.22bis 2400bps, 32=V.32)")
    common(sp)
    sp.set_defaults(fn=cmd_run_slmodem, peer="sip:11@10.42.0.102:5062;transport=udp")

    a = p.parse_args()
    a.fn(a)


if __name__ == "__main__":
    main()
