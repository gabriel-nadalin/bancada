"""RX burst/latency analysis (pure DSP; no call orchestration).

Takes a TX and an RX WAV, detects the burst cadence in each, and reports the
pipeline delay plus per-burst pairing/miss statistics.
"""

import math
import sys
import wave

from .paths import SRATE, WINDOW_MS


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


def estimate_delay(tx_env, rx_env, max_ms=500):
    """Pipeline delay (ms) via cross-correlation of Goertzel envelopes.

    Positive result = RX is delayed relative to TX.  The search is
    limited to ±``max_ms`` -- this is the *pipeline* delay, not the
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


def analyze_pair(tx_wav, rx_wav, freq, period, csv=None):
    """Analyze an RX WAV against a TX WAV. Returns process exit code
    (0 = all bursts accounted for, 2 = some bursts missing)."""
    tx_pcm = read_wav(tx_wav)
    rx_pcm = read_wav(rx_wav)

    tx = detect_bursts_cadence(tx_pcm, freq)
    rx = detect_bursts_cadence(rx_pcm, freq)
    print(f"tx bursts: {len(tx)}  rx bursts: {len(rx)}"
          "  (cadence cross-correlation)")

    # Pipeline delay via cross-correlation of Goertzel envelopes.
    tx_env = goertzel_envelope(tx_pcm, freq)
    rx_env = goertzel_envelope(rx_pcm, freq)
    pipeline_delay = estimate_delay(tx_env, rx_env, max(500, period * 2))
    print(f"pipeline delay: {pipeline_delay} ms"
          "  (cross-correlation of Goertzel envelope)")

    # Per-burst: pair each TX burst with the closest RX burst at
    # expected position (tx + pipeline_delay +/- margin).
    margin = period // 3  # generous to catch all
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

    for i, (t, r, delay) in enumerate(rows):
        r_str = f"{r} ms" if r is not None else "---    "
        d_str = f"delay={delay} ms" if delay is not None else "MISSING"
        print(f"burst {i:3d}: tx={t:>7} ms  rx={r_str:>8}  {d_str}")

    found = len(tx) - missed
    print(f"\nresult: pipeline delay = {pipeline_delay} ms"
          f"  (of {len(tx)} bursts: {found} received, {missed} missing)")

    if missed > len(tx) // 4:
        print("warning: >25% of bursts missing", file=sys.stderr)

    if csv:
        with open(csv, "w") as f:
            f.write("burst,tx_ms,rx_ms,delay_ms\n")
            for i, (t, r, d) in enumerate(rows):
                f.write(f"{i},{t},{r if r is not None else ''},"
                       f"{d if d is not None else ''}\n")
        print(f"wrote {csv}")

    return 2 if missed else 0
