"""TX burst-signal generation (pure DSP, no I/O orchestration)."""

import math
import wave

from .paths import SRATE, AMP


def pattern_sample(idx, freq, burst_ms, period_ms):
    """Sample value at global sample index (sine burst or silence)."""
    period = SRATE * period_ms // 1000
    burst = SRATE * burst_ms // 1000
    if idx % period < burst:
        return int(AMP * math.sin(2.0 * math.pi * freq * idx / SRATE))
    return 0


def generate_tx_wav(path, dur, freq, burst, period):
    """Write a mono s16/8 kHz WAV with the burst pattern.

    Returns the number of bursts written.
    """
    n = SRATE * dur
    buf = bytearray(n * 2)
    mv = memoryview(buf).cast("h")
    for i in range(n):
        mv[i] = pattern_sample(i, freq, burst, period)

    w = wave.open(path, "wb")
    w.setnchannels(1)
    w.setsampwidth(2)
    w.setframerate(SRATE)
    w.writeframes(bytes(buf))
    w.close()

    return dur * 1000 // period
