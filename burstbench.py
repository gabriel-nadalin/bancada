#!/usr/bin/env python3
"""burstbench - burst/delay benchmark for SIP audio paths.

Drives any tool that exchanges raw s16le/8kHz/mono audio, using sine
bursts:

  gen      create a TX wav with the burst pattern
  analyze  compare TX and RX wavs, report per-burst delay + stats
  live     real-time benchmark of a command with stdin/stdout PCM pipes
  run      orchestrate a full benchmark with baresip, pjsua, or rtp_bridge

Examples
  # Run a full benchmark with any tool:
  burstbench.py run baresip [--peer sip:...]
  burstbench.py run pjsua   [--peer sip:...]
  burstbench.py run bridge  [--peer sip:...] [--live]

  # Or manually (same thing):
  burstbench.py gen audios/bench_tx.wav
  ./baresip_play -p sip:... -d audios/bench_tx.wav -D audios/rx.wav
  burstbench.py analyze audios/bench_tx.wav audios/rx.wav
"""

import argparse
import math
import os
import select
import shlex
import signal
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


def burst_starts(dur_ms, warmup_ms, period_ms):
    t = warmup_ms
    while t < dur_ms:
        yield t
        t += period_ms


# ---------------------------------------------------------------- detector

class GoertzelDetector:
    """Goertzel power normalized by total window energy,
    with hangover-based onset detection"""

    def __init__(self, freq, thresh=0.25, hangover_win=3):
        self.coeff = 2.0 * math.cos(2.0 * math.pi * freq / SRATE)
        self.thresh = thresh
        self.hangover_win = hangover_win
        self.active = False
        self.below = 0
        self.onsets_ms = []
        self._clock_ms = 0

    def feed(self, samples):
        """feed one WINDOW_MS window of int samples; return True if onset"""
        nf = len(samples)
        s1 = s2 = 0.0
        sum_sq = 0
        for x in samples:
            s0 = x + self.coeff * s1 - s2
            s2 = s1
            s1 = s0
            sum_sq += x * x
        power = max(0.0, s2 * s2 + s1 * s1 - self.coeff * s1 * s2)
        norm = (2.0 * power) / (sum_sq * nf) if sum_sq > 0 and nf > 0 else 0.0
        onset = False
        if norm > self.thresh:
            if not self.active:
                self.active = True
                self.onsets_ms.append(self._clock_ms)
                onset = True
            self.below = 0
        elif self.active:
            self.below += 1
            if self.below >= self.hangover_win:
                self.active = False
        self._clock_ms += WINDOW_MS
        return onset


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


def detect_onsets(pcm_bytes, freq, thresh):
    det = GoertzelDetector(freq, thresh)
    wsz = SRATE * WINDOW_MS // 1000 * 2          # window size in bytes
    for off in range(0, len(pcm_bytes) - wsz + 1, wsz):
        det.feed(memoryview(pcm_bytes)[off:off + wsz].cast("h"))
    return det.onsets_ms


def summary(delays):
    n = len(delays)
    if not n:
        return "no bursts paired"
    mean = sum(delays) / n
    var = sum((d - mean) ** 2 for d in delays) / n
    return (f"n={n} mean={mean:.1f} ms min={min(delays):.1f} ms "
            f"max={max(delays):.1f} ms stdev={math.sqrt(var):.1f} ms")


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
    starts = list(burst_starts(a.dur * 1000, 0, a.period))
    print(f"{a.tx_wav}: {a.dur} s, {len(starts)} bursts "
          f"({a.freq} Hz, {a.burst} ms every {a.period} ms), "
          f"first at 0 ms", file=sys.stderr)


def cmd_analyze(a):
    tx = detect_onsets(read_wav(a.tx_wav), a.freq, a.thresh)
    rx = detect_onsets(read_wav(a.rx_wav), a.freq, a.thresh)
    print(f"tx bursts: {len(tx)}  rx bursts: {len(rx)}")

    delays = []
    rows = []
    for i, t in enumerate(tx):
        if i < len(rx):
            d = rx[i] - t
            delays.append(d)
            rows.append((i, t, rx[i], d))
        else:
            rows.append((i, t, None, None))
    for i, t, r, d in rows:
        if d is None:
            print(f"burst {i:3d}: tx={t:7d} ms  rx=   ---   MISSING")
        else:
            print(f"burst {i:3d}: tx={t:7d} ms  rx={r:7d} ms  delay={d} ms")
    missing = len(tx) - len(delays)
    print(f"\ndelay: {summary(delays)}" +
          (f"  missing={missing}" if missing else ""))
    print("note: includes each tool's constant pipeline start offset;"
          " compare stats between tools, not absolute zero")
    if a.csv:
        with open(a.csv, "w") as f:
            f.write("burst,tx_ms,rx_ms,delay_ms\n")
            for i, t, r, d in rows:
                f.write(f"{i},{t},{'' if r is None else r},"
                        f"{'' if d is None else d}\n")
        print(f"wrote {a.csv}")
    if missing:
        sys.exit(2)


def cmd_live(a):
    argv = shlex.split(a.cmd)
    print(f"live: spawning: {argv}", file=sys.stderr)
    child = subprocess.Popen(argv, stdin=subprocess.PIPE,
                             stdout=subprocess.PIPE, stderr=None)
    os.set_blocking(child.stdout.fileno(), False)

    frame_bytes = 16 * a.ptime                  # 8k s16 -> 16 bytes/ms
    frame_sampc = 8 * a.ptime
    win_bytes = 160                             # 10 ms
    dur_ms = a.dur * 1000
    warmup_ms = int(a.warmup * 1000)
    warmup_samp = 8 * warmup_ms
    period_samp = 8 * a.period

    det = GoertzelDetector(a.freq, a.thresh)
    rx_buf = bytearray()
    rx_wav = bytearray() if a.rec else None

    tx_starts = []                              # (start_ms, paired)
    rtts = []

    def pump_sample(idx):
        """waveform sample; silence until warmup, then burst pattern"""
        if idx < warmup_samp:
            return 0
        j = idx - warmup_samp
        if j % period_samp < 8 * a.burst:
            return int(AMP * math.sin(2.0 * math.pi * a.freq * j / SRATE))
        return 0

    t0 = time.monotonic()
    clock_ms = 0
    sample_idx = 0
    next_tick = t0

    try:
        while clock_ms < dur_ms:
            # drain child stdout until the next 20ms tick is due
            while True:
                now = time.monotonic()
                wait = next_tick - now
                if wait <= 0:
                    break
                r, _, _ = select.select([child.stdout], [], [], wait)
                if not r:
                    break
                chunk = os.read(child.stdout.fileno(), 65536)
                if not chunk:
                    clock_ms = dur_ms            # child closed: finish
                    break
                chunk_t_ms = (time.monotonic() - t0) * 1000.0
                rx_buf.extend(chunk)
                if rx_wav is not None:
                    rx_wav.extend(chunk)
                while len(rx_buf) >= win_bytes:
                    win = bytes(rx_buf[:win_bytes])
                    del rx_buf[:win_bytes]
                    if det.feed(memoryview(win).cast("h")):
                        # date the onset (start of triggering window) by
                        # wall-clock: chunk arrival minus the audio time
                        # still buffered after this window
                        onset = (chunk_t_ms
                                 - len(rx_buf) / 16.0 - WINDOW_MS)
                        # pair with oldest unpaired burst start
                        for j, (bs, used) in enumerate(tx_starts):
                            if not used and onset >= bs:
                                tx_starts[j] = (bs, True)
                                rtt = onset - bs
                                rtts.append(rtt)
                                print(f"burst {len(rtts):3d}: "
                                      f"tx={bs:6d} ms  rx={onset:7.1f} ms"
                                      f"  RTT={rtt:.1f} ms")
                                break
            if clock_ms >= dur_ms:
                break

            # write exactly one frame per tick
            frame = memoryview(bytearray(frame_bytes)).cast("h")
            for k in range(frame_sampc):
                frame[k] = pump_sample(sample_idx + k)
            try:
                child.stdin.write(frame.tobytes())
                child.stdin.flush()
            except BrokenPipeError:
                print("live: child closed stdin", file=sys.stderr)
                break
            # record the tx time of any burst starting inside this frame
            k_next = (max(0, sample_idx - warmup_samp + period_samp - 1)
                      // period_samp)
            sk = warmup_samp + k_next * period_samp
            if sample_idx <= sk < sample_idx + frame_sampc:
                tx_starts.append((clock_ms + (sk - sample_idx) // 8, False))
            sample_idx += frame_sampc
            clock_ms += a.ptime
            next_tick += a.ptime / 1000.0
    except KeyboardInterrupt:
        pass
    finally:
        try:
            child.stdin.close()
        except Exception:
            pass
        try:
            child.wait(timeout=5)
        except subprocess.TimeoutExpired:
            child.send_signal(signal.SIGINT)
            try:
                child.wait(timeout=3)
            except subprocess.TimeoutExpired:
                child.kill()

    missing = sum(1 for _, used in tx_starts if not used)
    print(f"\nlive RTT: {summary(rtts)}" +
          (f"  missing={missing}" if missing else ""))
    if a.rec:
        w = wave.open(a.rec, "wb")
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SRATE)
        w.writeframes(bytes(rx_wav))
        w.close()
        print(f"wrote {a.rec} ({len(rx_wav)} bytes)")
    if child.returncode not in (0, None):
        print(f"warning: child exited with {child.returncode}",
              file=sys.stderr)


# ---------------------------------------------------------------- run (orchestration)

def _ensure_tx_wav(freq, burst, period, dur=30):
    if not os.path.exists(TX_WAV):
        print(f"generating {TX_WAV} ...", file=sys.stderr)
        subprocess.check_call([sys.argv[0], "gen", TX_WAV,
                               "--dur", str(dur),
                               "--freq", str(freq),
                               "--burst", str(burst),
                               "--period", str(period)])


def _run_common(sp):
    sp.add_argument("--tx-wav", default=TX_WAV)
    sp.add_argument("--rx-wav", default=None)
    sp.add_argument("--peer", default=None)
    sp.add_argument("--dur", type=int, default=30)
    sp.add_argument("--ptime", type=int, default=20)


def _run_analyze(a, rx):
    subprocess.check_call(
        [sys.argv[0], "analyze", a.tx_wav, rx,
         "--freq", str(a.freq), "--thresh", str(a.thresh),
         "--burst", str(a.burst), "--period", str(a.period)])


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
    subprocess.check_call(cmd)
    _run_analyze(a, rx)


def cmd_run_bridge(a):
    if a.live:
        _ensure_tx_wav(a.freq, a.burst, a.period, a.dur)
        bridge_cmd = shlex.join(
            [os.path.join(HERE, "rtp_bridge"),
             "-p", a.peer, "-t", str(a.ptime)])
        live_argv = [sys.argv[0], "live", "--cmd", bridge_cmd,
                     "--dur", str(a.dur), "--ptime", str(a.ptime),
                     "--freq", str(a.freq), "--thresh", str(a.thresh),
                     "--burst", str(a.burst), "--period", str(a.period)]
        print(f"+ {shlex.join(live_argv)}", file=sys.stderr)
        subprocess.check_call(live_argv)
    else:
        _ensure_tx_wav(a.freq, a.burst, a.period, a.dur)
        rx = a.rx_wav or os.path.join(AUDIOS, "bridge_rx.wav")
        cmd = [os.path.join(HERE, "rtp_bridge"),
               "-p", a.peer, "-i", a.tx_wav, "-o", rx,
               "-t", str(a.ptime)]
        print(f"+ {shlex.join(cmd)}", file=sys.stderr)
        subprocess.check_call(cmd)
        _run_analyze(a, rx)


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
        sp.add_argument("--thresh", type=float, default=0.25,
                        help="Goertzel norm threshold (default 0.25)")

    g = sub.add_parser("gen", help="create TX burst wav")
    g.add_argument("tx_wav")
    g.add_argument("--dur", type=int, default=30, help="seconds (default 30)")
    common(g)
    g.set_defaults(fn=cmd_gen)

    an = sub.add_parser("analyze", help="pair TX/RX wavs, report delays")
    an.add_argument("tx_wav")
    an.add_argument("rx_wav")
    an.add_argument("--csv")
    common(an)
    an.set_defaults(fn=cmd_analyze)

    li = sub.add_parser("live", help="live benchmark of a PCM-pipe command")
    li.add_argument("--cmd", required=True,
                    help="command to run, e.g. './rtp_bridge -p sip:...'")
    li.add_argument("--dur", type=int, default=30)
    li.add_argument("--warmup", type=float, default=1.0,
                    help="seconds of silence before first burst")
    li.add_argument("--ptime", type=int, default=20,
                    help="frame size in ms (match the tool; default 20)")
    li.add_argument("--rec", help="record received PCM to wav")
    common(li)
    li.set_defaults(fn=cmd_live)

    # run (orchestration)
    ru = sub.add_parser("run", help="run a full benchmark with a tool")
    rsub = ru.add_subparsers(dest="tool", required=True)

    bp = rsub.add_parser("baresip", help="run dial_and_play benchmark")
    _run_common(bp)
    common(bp)
    bp.set_defaults(fn=cmd_run_baresip, peer="sip:11@10.42.0.102:5062;transport=udp")

    pp = rsub.add_parser("pjsua", help="run pjsua benchmark")
    _run_common(pp)
    pp.add_argument("--auto-answer", action="store_true",
                    help="auto-answer incoming calls")
    common(pp)
    pp.set_defaults(fn=cmd_run_pjsua, peer="sip:11@10.42.0.102:5062;transport=udp", ptime=10)

    bpr = rsub.add_parser("bridge", help="run rtp_bridge benchmark")
    _run_common(bpr)
    bpr.add_argument("--live", action="store_true",
                     help="live stdin/stdout mode vs file-based")
    common(bpr)
    bpr.set_defaults(fn=cmd_run_bridge, peer="sip:11@10.42.0.102:5062;transport=udp")

    a = p.parse_args()
    a.fn(a)


if __name__ == "__main__":
    main()
