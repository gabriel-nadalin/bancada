#!/usr/bin/env python3
"""Reproduce the V.90 spectral diagnosis from bench captures and simulations."""

from __future__ import annotations

import argparse
import csv
import hashlib
import math
import re
import wave
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np
from scipy import signal

import fixedrc


FS_IO = 8000
FS_DSP = 9600
FFT_SIZE = 1024
PSD_LENGTH = 4096
PSD_OVERLAP = 512
PRE_ROLL = 8000
TEST_FREQUENCIES = (3600.0, 4000.0, 4200.0)
SEVERE_THRESHOLD_DB = 28.0
TRN1D_ERROR_THRESHOLD = 180.0
ERROR_BLOCK_SYMBOLS = 160
ERROR_IIR_ALPHA = 0.7
EVALUATION_SYMBOLS = 1600
CAPTURE_HASHES = {
    "e1_trn1d_preroll.wav": "d52e3d3c8317e6ce34d8edeaf900a664a52a0aa96280f7248b13f28c9e5ee796",
    "sip_da_ad_trn1d_preroll.wav": "e7c8e374f21d9419e135e36ef552e988e8f6548055e1c27efce7687d4cd40e95",
    "sip_digital_trn1d_preroll.wav": "64821a386e17714a91397fbb736867267db093c39bae3f75e41c3d43dd12a375",
}
FULL_ARTIFACT_HASHES = {
    "sip_digital_v90_rx_full.wav": "6141c3f419e33194ef5095a56e704f8bc69f43281879a06ecf7b58cc51027ed2",
    "sip_digital_v90_tx_full.wav": "300f444d7d2cb786bd142d210c884a4ecd0b28ea218e53ddaf09a9aa834fce80",
    "sip_digital_v90.log": "56ca37d3b9bb46c087a753bbbf7bb8d3533ed7e77ee47bca45ab1a35ef0c4e16",
}


def arguments() -> argparse.Namespace:
    analysis_root = Path(__file__).resolve().parent
    repository_root = analysis_root.parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--data", type=Path, default=analysis_root / "data")
    parser.add_argument("--results", type=Path, default=analysis_root / "results")
    parser.add_argument(
        "--figures",
        type=Path,
        default=repository_root / "docs" / "figures",
        help="Diretório de saída das figuras usadas pelo relatório LaTeX",
    )
    return parser.parse_args()


def read_wav(path: Path) -> np.ndarray:
    with wave.open(str(path), "rb") as wav:
        if wav.getnchannels() != 1 or wav.getframerate() != FS_IO or wav.getsampwidth() != 2:
            raise ValueError(f"Expected mono 16-bit PCM at 8000 sample/s: {path}")
        return np.frombuffer(wav.readframes(wav.getnframes()), dtype="<i2").astype(float)


def verify_capture_hashes(data: Path) -> None:
    for name, expected in {**CAPTURE_HASHES, **FULL_ARTIFACT_HASHES}.items():
        actual = hashlib.sha256((data / name).read_bytes()).hexdigest()
        if actual != expected:
            raise ValueError(f"Capture hash mismatch for {name}: {actual}")


def rcfixed_8_to_9_6(samples: np.ndarray, fixedrc) -> np.ndarray:
    handle = fixedrc.RcFixed_Create(fixedrc.RcRatioCode.RC_8K_TO_9_6K)
    output, count = fixedrc.RcFixed_Resample(
        handle, np.rint(samples).astype(np.int16).tolist(), 2 * len(samples)
    )
    return np.asarray(output[:count], dtype=float)


def dsplibs_psd(samples: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    """Implement Psd::process, including its non-periodic Hann definition."""
    if len(samples) != PSD_LENGTH:
        raise ValueError(f"PSD input must contain {PSD_LENGTH} samples")
    k = np.arange(FFT_SIZE)
    window = 0.5 * (1.0 - np.cos(2.0 * np.pi * (k + 1.0) / (FFT_SIZE + 1.0)))
    powers = []
    step = FFT_SIZE - PSD_OVERLAP
    frame_count = (PSD_LENGTH - PSD_OVERLAP) // step
    for frame in range(frame_count):
        start = frame * step
        spectrum = np.fft.rfft(samples[start : start + FFT_SIZE] * window)
        powers.append(np.abs(spectrum[: FFT_SIZE // 2]) ** 2)
    power_db = 10.0 * np.log10(np.mean(powers, axis=0) + 1.0e-25)
    frequency = np.arange(FFT_SIZE // 2) * FS_DSP / FFT_SIZE
    return frequency, power_db


def nearest_bin_values(power_db: np.ndarray) -> np.ndarray:
    bin_hz = FS_DSP / FFT_SIZE
    return np.asarray([power_db[round(frequency / bin_hz)] for frequency in TEST_FREQUENCIES])


def verifier_result(values: np.ndarray) -> tuple[np.ndarray, bool]:
    drops = values[0] - values[1:]
    return drops, bool(np.all(drops > SEVERE_THRESHOLD_DB))


def trn1d(length: int) -> np.ndarray:
    """Generate the antipodal sequence made by the V.90 18/23 scrambler."""
    output_bits: list[int] = []
    for index in range(length):
        delayed_18 = output_bits[index - 18] if index >= 18 else 0
        delayed_23 = output_bits[index - 23] if index >= 23 else 0
        output_bits.append(1 ^ delayed_18 ^ delayed_23)
    return 2.0 * np.asarray(output_bits) - 1.0


def fractional_correlation_delay(reference: np.ndarray, observed: np.ndarray) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    times: list[float] = []
    delays: list[float] = []
    chunk_length = 1024
    stride = 256
    for start in range(0, len(reference) - chunk_length + 1, stride):
        x = reference[start : start + chunk_length]
        y = observed[start : start + chunk_length]
        correlation = signal.correlate(y - np.mean(y), x - np.mean(x), mode="full", method="fft")
        lags = signal.correlation_lags(len(y), len(x), mode="full")
        keep = (lags >= 10) & (lags <= 30)
        correlation = correlation[keep]
        lags = lags[keep]
        peak = int(np.argmax(correlation))
        delay = float(lags[peak])
        if 0 < peak < len(correlation) - 1:
            denominator = correlation[peak - 1] - 2.0 * correlation[peak] + correlation[peak + 1]
            if denominator != 0.0:
                delay += 0.5 * (correlation[peak - 1] - correlation[peak + 1]) / denominator
        times.append((start + chunk_length / 2.0) / FS_IO)
        delays.append(delay)
    coefficients = np.polyfit(times, delays, 1)
    return np.asarray(times), np.asarray(delays), coefficients


def local_channel_models(reference: np.ndarray, observed: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    """Fit short local FIR models; locality prevents clock drift from smearing the response."""
    chunk_length = 1536
    stride = 256
    tap_count = 80
    models: list[np.ndarray] = []
    scores: list[float] = []
    for start in range(0, len(reference) - chunk_length + 1, stride):
        x = reference[start : start + chunk_length]
        y = observed[start : start + chunk_length]
        design = np.column_stack(
            [np.r_[np.zeros(tap), x[: len(x) - tap]] for tap in range(tap_count)]
        )
        train = np.arange(chunk_length) % 4 != 3
        test = ~train
        taps = np.linalg.lstsq(design[train], y[train], rcond=None)[0]
        prediction = design[test] @ taps
        residual = y[test] - prediction
        r_squared = 1.0 - np.sum(residual**2) / np.sum((y[test] - np.mean(y[test])) ** 2)
        models.append(taps)
        scores.append(float(r_squared))
    return np.asarray(models), np.asarray(scores)


def normalized_channel_responses(models: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    frequency = np.fft.rfftfreq(16384, d=1.0 / FS_IO)
    magnitude_db = 20.0 * np.log10(np.abs(np.fft.rfft(models, 16384, axis=1)) + 1.0e-30)
    reference_levels = np.asarray([np.interp(3600.0, frequency, row) for row in magnitude_db])
    return frequency, magnitude_db - reference_levels[:, np.newaxis]


def exact_fixedrc_response(fixedrc) -> tuple[np.ndarray, np.ndarray]:
    phase_bank = np.asarray(fixedrc.rc80to96filter, dtype=float).reshape(6, 36)
    # RcFixed stores six contiguous branches in descending fractional-phase order.
    prototype = phase_bank[::-1].T.reshape(-1)
    frequency, response = signal.freqz(prototype, worN=32768, fs=48000)
    magnitude_db = 20.0 * np.log10(np.abs(response) + 1.0e-30)
    magnitude_db -= magnitude_db[0]
    return frequency, magnitude_db


def write_csv(path: Path, header: list[str], rows: list[list[object]]) -> None:
    with path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.writer(stream, lineterminator="\n")
        writer.writerow(header)
        writer.writerows(rows)


def configure_plots() -> None:
    plt.rcParams.update(
        {
            "figure.figsize": (7.0, 4.3),
            "font.family": "Fira Sans",
            "font.size": 9.5,
            "axes.grid": True,
            "grid.alpha": 0.25,
            "axes.spines.top": False,
            "axes.spines.right": False,
            "savefig.bbox": "tight",
        }
    )


def save_figure(figure: plt.Figure, figures: Path, stem: str) -> None:
    figures.mkdir(parents=True, exist_ok=True)
    figure.savefig(figures / f"{stem}.pdf")
    plt.close(figure)


def bench_analysis(data: Path, results: Path, figures: Path) -> dict[str, object]:
    verify_capture_hashes(data)
    e1_all = read_wav(data / "e1_trn1d_preroll.wav")
    sip_all = read_wav(data / "sip_da_ad_trn1d_preroll.wav")
    sip_digital_all = read_wav(data / "sip_digital_trn1d_preroll.wav")
    if any(len(samples) != 24000 for samples in (e1_all, sip_all, sip_digital_all)):
        raise ValueError("The checked-in excerpts must each contain exactly 24000 samples")

    e1_9_6 = rcfixed_8_to_9_6(e1_all, fixedrc)
    sip_9_6 = rcfixed_8_to_9_6(sip_all, fixedrc)
    sip_digital_9_6 = rcfixed_8_to_9_6(sip_digital_all, fixedrc)
    verifier_start = PRE_ROLL * FS_DSP // FS_IO
    f_e1, p_e1 = dsplibs_psd(e1_9_6[verifier_start : verifier_start + PSD_LENGTH])
    f_sip, p_sip = dsplibs_psd(sip_9_6[verifier_start : verifier_start + PSD_LENGTH])
    f_sip_digital, p_sip_digital = dsplibs_psd(
        sip_digital_9_6[verifier_start : verifier_start + PSD_LENGTH]
    )
    e1_values = nearest_bin_values(p_e1)
    sip_values = nearest_bin_values(p_sip)
    sip_digital_values = nearest_bin_values(p_sip_digital)
    e1_drops, e1_severe = verifier_result(e1_values)
    sip_drops, sip_severe = verifier_result(sip_values)
    sip_digital_drops, sip_digital_severe = verifier_result(sip_digital_values)
    if np.max(np.abs(e1_values - np.asarray([96.22, 76.18, 47.50]))) > 0.011:
        raise AssertionError("E1 capture no longer reproduces the decoded log")
    if np.max(np.abs(sip_values - np.asarray([92.19, 52.00, 33.76]))) > 0.011:
        raise AssertionError("SIP capture no longer reproduces the decoded log")
    if np.max(np.abs(sip_digital_values - np.asarray([96.2291, 76.1984, 47.5015]))) > 0.001:
        raise AssertionError("Digital SIP capture no longer reproduces the archived measurement")
    if e1_severe or not sip_severe or sip_digital_severe:
        raise AssertionError("Unexpected spectral-verifier decisions")

    e1 = e1_all[PRE_ROLL : PRE_ROLL + 12000]
    sip = sip_all[PRE_ROLL : PRE_ROLL + 12000]
    # The successful digital-SIP run has the opposite network polarity.  The
    # sign is immaterial to the spectrum and equalizer, but normalize it so
    # clock and channel comparisons use the same TRN1d reference polarity.
    sip_digital = -sip_digital_all[PRE_ROLL : PRE_ROLL + 12000]
    generated = trn1d(len(e1) + 2000)
    correlations = np.asarray(
        [np.mean((e1 / 3904.0) * generated[offset : offset + len(e1)]) for offset in range(2000)]
    )
    trn_offset = int(np.argmax(correlations))
    trn_correlation = float(correlations[trn_offset])
    if trn_offset != 135 or not math.isclose(trn_correlation, 1.0, abs_tol=1.0e-12):
        raise AssertionError("E1 capture does not exactly match the reconstructed TRN1d")
    digital_correlations = np.asarray(
        [np.mean((sip_digital / 3904.0) * generated[offset : offset + len(sip_digital)]) for offset in range(2000)]
    )
    sip_digital_trn_offset = int(np.argmax(digital_correlations))
    sip_digital_trn_correlation = float(digital_correlations[sip_digital_trn_offset])
    if sip_digital_trn_offset != 125 or not math.isclose(
        sip_digital_trn_correlation, 1.0, abs_tol=1.0e-12
    ):
        raise AssertionError("Digital SIP capture does not exactly match the reconstructed TRN1d")

    times, delays, delay_fit = fractional_correlation_delay(e1, sip)
    delay_prediction = np.polyval(delay_fit, times)
    clock_ppm = delay_fit[0] / FS_IO * 1.0e6
    clock_rmse = float(np.sqrt(np.mean((delays - delay_prediction) ** 2)))

    digital_times, digital_delays, digital_delay_fit = fractional_correlation_delay(
        e1, sip_digital
    )
    digital_delay_prediction = np.polyval(digital_delay_fit, digital_times)
    sip_digital_clock_ppm = digital_delay_fit[0] / FS_IO * 1.0e6
    sip_digital_clock_rmse = float(
        np.sqrt(np.mean((digital_delays - digital_delay_prediction) ** 2))
    )
    if abs(sip_digital_clock_ppm) > 1.0e-6 or sip_digital_clock_rmse > 1.0e-9:
        raise AssertionError("Digital SIP TRN1d unexpectedly drifts relative to direct E1")

    models, model_scores = local_channel_models(e1, sip)
    channel_frequency, channel_responses = normalized_channel_responses(models)
    channel_median = np.median(channel_responses, axis=0)
    channel_q25, channel_q75 = np.percentile(channel_responses, [25, 75], axis=0)
    loss_3800 = -float(np.interp(3800.0, channel_frequency, channel_median))
    loss_3996 = -float(np.interp(3996.0, channel_frequency, channel_median))
    if np.median(model_scores) < 0.95:
        raise AssertionError("The local channel model no longer explains the SIP capture")

    digital_models, digital_model_scores = local_channel_models(e1, sip_digital)
    digital_channel_frequency, digital_channel_responses = normalized_channel_responses(
        digital_models
    )
    digital_channel_median = np.median(digital_channel_responses, axis=0)
    digital_channel_q25, digital_channel_q75 = np.percentile(
        digital_channel_responses, [25, 75], axis=0
    )
    sip_digital_loss_3800 = -float(
        np.interp(3800.0, digital_channel_frequency, digital_channel_median)
    )
    sip_digital_loss_3996 = -float(
        np.interp(3996.0, digital_channel_frequency, digital_channel_median)
    )
    if np.median(digital_model_scores) < 0.99:
        raise AssertionError("The digital SIP channel model no longer matches direct E1")

    write_csv(
        results / "verifier_results.csv",
        ["path", "p_3600_db", "p_4000_db", "p_4200_db", "drop_4000_db", "drop_4200_db", "severe"],
        [
            ["E1 digital", *e1_values, *e1_drops, e1_severe],
            ["SIP digital", *sip_digital_values, *sip_digital_drops, sip_digital_severe],
            ["SIP D/A-A/D", *sip_values, *sip_drops, sip_severe],
        ],
    )
    write_csv(
        results / "clock_fit.csv",
        ["path", "time_s", "delay_samples", "fitted_delay_samples"],
        [
            *[["SIP D/A-A/D", time, delay, fit] for time, delay, fit in zip(times, delays, delay_prediction)],
            *[
                ["SIP digital", time, delay, fit]
                for time, delay, fit in zip(
                    digital_times, digital_delays, digital_delay_prediction
                )
            ],
        ],
    )
    write_csv(
        results / "channel_response.csv",
        ["path", "frequency_hz", "q25_db_rel_3600", "median_db_rel_3600", "q75_db_rel_3600"],
        [
            *[["SIP D/A-A/D", frequency, low, median, high]
            for frequency, low, median, high in zip(
                channel_frequency, channel_q25, channel_median, channel_q75
            )],
            *[["SIP digital", frequency, low, median, high]
            for frequency, low, median, high in zip(
                digital_channel_frequency,
                digital_channel_q25,
                digital_channel_median,
                digital_channel_q75,
            )],
        ],
    )

    fig, ax = plt.subplots(figsize=(7.0, 3.6), constrained_layout=True)
    ax.step(np.arange(80) / FS_IO * 1000.0, e1[:80] / 3904.0, where="post", color="#1f77b4")
    ax.set(
        xlabel="Tempo (ms)",
        ylabel="Nível normalizado",
        title="TRN1d capturada no E1: um símbolo antipodal por amostra a 8 kHz",
    )
    save_figure(fig, figures, "trn1d_samples")

    rc_frequency, rc_magnitude = exact_fixedrc_response(fixedrc)
    rc_points = np.asarray([np.interp(value, rc_frequency, rc_magnitude) for value in TEST_FREQUENCIES])
    if np.max(np.abs(rc_points - np.asarray([-2.15, -20.53, -48.33]))) > 0.05:
        raise AssertionError("Unexpected RcFixed prototype response")
    fig, ax = plt.subplots(figsize=(7.0, 3.8), constrained_layout=True)
    ax.plot(rc_frequency, rc_magnitude, color="#2ca02c", label="Protótipo exato RcFixed")
    for frequency in TEST_FREQUENCIES:
        ax.axvline(frequency, color="0.65", linewidth=0.8, linestyle="--")
    ax.set(
        xlim=(0, 4800),
        ylim=(-70, 2),
        xlabel="Frequência (Hz)",
        ylabel="Magnitude (dB)",
        title="Resposta do RcFixed de 8,0 para 9,6 kamostras/s",
    )
    ax.legend()
    save_figure(fig, figures, "rcfixed_response")

    normalized_e1 = p_e1 - e1_values[0]
    normalized_sip_digital = p_sip_digital - sip_digital_values[0]
    if not np.array_equal(f_e1, f_sip_digital):
        raise AssertionError("Digital spectra do not share the same frequency grid")

    fig, ax = plt.subplots(constrained_layout=True)
    ax.plot(
        f_e1,
        0.5 * (normalized_e1 + normalized_sip_digital),
        label="E1 e SIP digital (sobrepostos)",
        color="#1f77b4",
        linewidth=1.8,
    )
    ax.plot(f_sip, p_sip - sip_values[0], label="SIP D/A–A/D", color="#d62728")
    for frequency in TEST_FREQUENCIES:
        ax.axvline(frequency, color="0.55", linewidth=0.8, linestyle="--")
    ax.axhline(-SEVERE_THRESHOLD_DB, color="black", linewidth=1.0, linestyle=":", label="limiar de −28 dB")
    ax.set(
        xlim=(3000, 4500),
        ylim=(-75, 8),
        xlabel="Frequência (Hz)",
        ylabel="PSD relativa ao bin de 3600 Hz (dB)",
        title="Entrada exata do V90SpectralVerifier e estimador de Welch",
    )
    ax.legend(loc="lower left")
    save_figure(fig, figures, "measured_spectra")

    fig, ax = plt.subplots(figsize=(7.0, 4.0), constrained_layout=True)
    ax.fill_between(
        channel_frequency,
        channel_q25,
        channel_q75,
        color="#ff7f0e",
        alpha=0.25,
        label="Intervalo interquartil",
    )
    ax.plot(channel_frequency, channel_median, color="#d62728", label="Mediana SIP D/A–A/D")
    ax.plot(
        digital_channel_frequency,
        digital_channel_median,
        color="#2ca02c",
        linestyle="--",
        label="Mediana SIP digital",
    )
    ax.set(
        xlim=(3000, 4000),
        ylim=(-35, 8),
        xlabel="Frequência de origem (Hz)",
        ylabel="Ganho adicional relativo a 3600 Hz (dB)",
        title="Resposta medida do caminho D/A–A/D",
    )
    ax.legend(loc="lower left")
    save_figure(fig, figures, "channel_response")

    fig, ax = plt.subplots(figsize=(7.0, 4.0), constrained_layout=True)
    ax.scatter(times, delays, s=13, color="#1f77b4", label="Correlação por bloco")
    ax.plot(times, delay_prediction, color="black", label=f"Ajuste: {clock_ppm:.1f} ppm")
    ax.scatter(
        digital_times,
        digital_delays,
        s=12,
        color="#2ca02c",
        marker="x",
        label="Blocos SIP digital",
    )
    ax.plot(
        digital_times,
        digital_delay_prediction,
        color="#2ca02c",
        linestyle="--",
        label=f"Ajuste digital: {sip_digital_clock_ppm:.1f} ppm",
    )
    ax.set(
        xlabel="Tempo na sequência TRN1d (s)",
        ylabel="Atraso SIP relativo ao E1 (amostras)",
        title="Deriva linear de atraso entre relógios assíncronos",
    )
    ax.legend()
    save_figure(fig, figures, "clock_drift")

    fig, ax = plt.subplots(constrained_layout=True)
    positions = np.arange(2)
    width = 0.25
    ax.bar(positions - width, e1_drops, width, label="E1 digital", color="#1f77b4")
    ax.bar(positions, sip_digital_drops, width, label="SIP digital", color="#2ca02c")
    ax.bar(positions + width, sip_drops, width, label="SIP D/A–A/D", color="#d62728")
    ax.axhline(SEVERE_THRESHOLD_DB, color="black", linestyle="--", label="Limiar de 28 dB")
    ax.set_xticks(positions, ["3600→4000 Hz", "3600→4200 Hz"])
    ax.set(
        ylabel="Queda de potência medida (dB)",
        title="O detector exige que ambas as quedas excedam o limiar",
    )
    ax.legend()
    save_figure(fig, figures, "verifier_budget")

    return {
        "e1_values": e1_values,
        "sip_values": sip_values,
        "sip_digital_values": sip_digital_values,
        "e1_drops": e1_drops,
        "sip_drops": sip_drops,
        "sip_digital_drops": sip_digital_drops,
        "trn_offset": trn_offset,
        "trn_correlation": trn_correlation,
        "sip_digital_trn_offset": sip_digital_trn_offset,
        "sip_digital_trn_correlation": sip_digital_trn_correlation,
        "clock_ppm": clock_ppm,
        "clock_rmse": clock_rmse,
        "sip_digital_clock_ppm": sip_digital_clock_ppm,
        "sip_digital_clock_rmse": sip_digital_clock_rmse,
        "models": models,
        "model_scores": model_scores,
        "digital_models": digital_models,
        "digital_model_scores": digital_model_scores,
        "loss_3800": loss_3800,
        "loss_3996": loss_3996,
        "sip_digital_loss_3800": sip_digital_loss_3800,
        "sip_digital_loss_3996": sip_digital_loss_3996,
        "rc_points": rc_points,
        "e1": e1,
        "sip": sip,
        "sip_digital": sip_digital,
    }


def time_warp(samples: np.ndarray, ppm: float) -> np.ndarray:
    output_length = int(round(len(samples) * (1.0 + ppm * 1.0e-6)))
    warped = signal.resample(samples, output_length)
    if len(warped) >= len(samples):
        return warped[: len(samples)]
    return np.pad(warped, (0, len(samples) - len(warped)), mode="edge")


def clock_warp_interpolation(samples: np.ndarray, ppm: float) -> np.ndarray:
    """Apply a deterministic sampling-clock offset while retaining the record length."""
    source_time = np.arange(len(samples), dtype=float) / (1.0 + ppm * 1.0e-6)
    return np.interp(
        source_time,
        np.arange(len(samples), dtype=float),
        samples,
        left=float(samples[0]),
        right=float(samples[-1]),
    )


def coarse_delay(reference: np.ndarray, observed: np.ndarray) -> int:
    """Find the initial integer delay before the adaptive equalizer starts."""
    length = min(3000, len(reference), len(observed))
    correlation = signal.correlate(
        observed[:length] - np.mean(observed[:length]),
        reference[:length] - np.mean(reference[:length]),
        mode="full",
        method="fft",
    )
    lags = signal.correlation_lags(length, length, mode="full")
    keep = (lags >= -50) & (lags <= 100)
    return int(lags[keep][np.argmax(correlation[keep])])


def simulate_v90_equalizer_core(
    reference: np.ndarray, observed: np.ndarray, symbol_count: int = 10000
) -> dict[str, np.ndarray | int | float]:
    """Reproduce the V.90 LE/DFE topology and the exact avePdsnr recurrence.

    This intentionally exposes only the mechanism under study.  It uses the
    reconstructed 300-tap, two-samples/symbol LE; 12-tap DFE; center-tap
    initialization; TRN1d beta schedule; 160-symbol RMS blocks; and alpha=0.7
    IIR.  The opaque prefilter, decision mapper, and timing loop are not
    claimed to be bit-exact here, so the absolute result is validated against
    the controlled binary run rather than fitted to it.
    """
    delay = coarse_delay(reference, observed)
    twice_rate = signal.resample_poly(observed, 2, 1)
    symbol_count = min(symbol_count, len(reference))
    le_coefficients = np.zeros(300, dtype=float)
    le_coefficients[150] = 1.0
    dfe_coefficients = np.zeros(12, dtype=float)
    dfe_history = np.zeros(12, dtype=float)
    error_energy = 0.0
    filtered_error = 0.0
    block_symbol: list[int] = []
    block_rms: list[float] = []
    block_filtered: list[float] = []

    for symbol_index in range(symbol_count):
        history_end = 2 * (symbol_index + delay) + 150
        if history_end >= len(twice_rate):
            break
        history = twice_rate[history_end - np.arange(300)]
        decision = reference[symbol_index]
        le_output = float(le_coefficients @ history)
        dfe_output = float(dfe_coefficients @ dfe_history)
        soft_decision = le_output - dfe_output
        error = soft_decision - decision
        le_error = le_output - decision

        # The reconstructed defaults enable the LE at symbol 1000 and the DFE
        # at symbol 2000 during TRN1d.
        if symbol_index >= 1000:
            le_coefficients += (-1.0e-10 * le_error) * history
        if symbol_index >= 2000:
            dfe_coefficients += (7.0e-9 * error) * dfe_history
        dfe_history[1:] = dfe_history[:-1]
        dfe_history[0] = le_error

        # ErrorEnergyAccum is a 32-bit integer in the reconstructed object;
        # the float path truncates each non-negative squared error before the
        # 160-symbol block RMS is formed.
        error_energy += math.trunc(error * error)
        if (symbol_index + 1) % ERROR_BLOCK_SYMBOLS == 0:
            rms_error = math.sqrt(error_energy / ERROR_BLOCK_SYMBOLS)
            filtered_error = (
                (1.0 - ERROR_IIR_ALPHA) * rms_error
                + ERROR_IIR_ALPHA * filtered_error
            )
            block_symbol.append(symbol_index + 1)
            block_rms.append(rms_error)
            block_filtered.append(filtered_error)
            error_energy = 0.0

    filtered = np.asarray(block_filtered)
    evaluation_blocks = EVALUATION_SYMBOLS // ERROR_BLOCK_SYMBOLS
    if len(filtered) < evaluation_blocks:
        raise ValueError("Equalizer simulation is shorter than the evaluation interval")
    # updateAvePdsnr weights every block by its symbol count.  All complete
    # blocks contain 160 symbols, hence this is exactly their arithmetic mean.
    ave_pdsnr = float(np.mean(filtered[-evaluation_blocks:]))
    return {
        "delay": delay,
        "symbol": np.asarray(block_symbol),
        "rms": np.asarray(block_rms),
        "filtered": filtered,
        "ave_pdsnr": ave_pdsnr,
    }


def digital_sip_error_trace(data: Path) -> tuple[np.ndarray, np.ndarray]:
    """Extract the successful digital-SIP TRN1d trajectory from its archived log."""
    text = (data / "sip_digital_v90.log").read_text(encoding="utf-8")
    required_evidence = (
        "Current data rate: TX=28800 RX=56000 bit/s",
        "[pty] response 1/3 passed",
        "[pty] response 2/3 passed",
        "[pty] response 3/3 passed",
        "[pty] PASS: 3 end-to-end RAS responses",
        "RTP TX packets=6092",
        "RTP RX pt=8 pkts=6092 payload 80..80 B",
        "ts_unexpected=0",
    )
    missing = [entry for entry in required_evidence if entry not in text]
    if missing:
        raise ValueError(f"Digital SIP log is missing required evidence: {missing}")
    sections = text.split("V90Equalizer: DfeProtectionOnDil")
    if len(sections) < 3:
        raise ValueError("Expected two V.90 attempts in the digital SIP log")
    successful = sections[-1].split(
        "V90Equalizer: ph4MeanErrorEnergyBeforeToAfterUpdateRatio", 1
    )[0]
    values = np.asarray(
        [
            float(value)
            for value in re.findall(
                r"V90Demodulator: Error Energy = ([+-]?\d+(?:\.\d+)?)",
                successful,
            )
        ]
    )
    starts = np.flatnonzero(values > 100.0)
    if len(starts) == 0:
        raise ValueError("Cannot locate the decision-directed transient")
    values = values[int(starts[0]) :]
    if len(values) < 30 or not np.all(values[-10:] < 10.0):
        raise ValueError("Digital SIP error trajectory does not converge as expected")
    times = 0.040 + 0.080 * np.arange(len(values))
    return times, values


def bench_error_traces(data: Path) -> dict[str, tuple[np.ndarray, np.ndarray]]:
    """Values printed by immutable bench logs during TRN1d DD."""
    e1_values = np.asarray(
        [
            961.081, 324.288, 120.404, 75.929, 51.752, 49.466, 43.226,
            45.374, 48.504, 41.445, 27.963, 22.151, 20.363, 17.555,
            16.863, 17.188, 15.653, 14.264, 13.765, 14.081, 11.978,
            12.221, 12.214, 12.164, 12.021, 12.238, 11.120, 11.942,
            12.041, 11.812, 12.169, 12.108, 10.860, 10.013, 11.451,
            11.118, 9.510, 11.826, 11.452, 11.752, 9.853, 10.822,
            11.305, 10.881, 11.236, 9.823, 11.107, 9.901,
        ]
    )
    sip_values = np.asarray(
        [
            948.323, 509.662, 391.813, 358.101, 336.701, 344.376,
            328.955, 342.838, 338.279, 330.968, 344.541, 347.932,
            346.828, 338.239, 333.195, 357.007, 346.329, 348.827,
            358.448, 337.461, 335.607, 329.089, 329.934, 355.457,
            349.698, 339.143, 353.412, 354.631, 359.749, 346.482,
            356.336, 356.313,
        ]
    )
    # The first reports occur 40.091 and 70.019 ms after the respective DD
    # transition; subsequent reports are separated by the configured 768
    # input samples, or 80 ms at 9600 sample/s.
    e1_time = 0.040091 + 0.080 * np.arange(len(e1_values))
    sip_time = 0.070019 + 0.080 * np.arange(len(sip_values))
    return {
        "E1 digital": (e1_time, e1_values),
        "SIP digital": digital_sip_error_trace(data),
        "SIP D/A-A/D, verifier bypassed": (sip_time, sip_values),
    }


def ave_pdsnr_simulation(
    bench: dict[str, object], data: Path, results: Path, figures: Path
) -> dict[str, float]:
    """Compare convergence and exact avePdsnr evaluation for controlled paths."""
    reference = np.asarray(bench["e1"])
    captured = np.asarray(bench["sip"])
    digital_captured = np.asarray(bench["sip_digital"])
    models = np.asarray(bench["models"])
    model_scores = np.asarray(bench["model_scores"])
    representative = models[np.argsort(model_scores)[len(model_scores) // 2]]
    filtered = signal.lfilter(representative, [1.0], reference)

    # The waveform measurement is -114.1 ppm, whereas the failed modem run
    # had acquired only -2.398 ppm.  The untracked residual is therefore
    # approximately -111.7 ppm.
    acquired_ppm = -2.398
    residual_ppm = float(bench["clock_ppm"]) - acquired_ppm
    cases = [
        ("Digital, synchronous", reference),
        ("Measured SIP digital capture", digital_captured),
        ("Digital, residual clock only", clock_warp_interpolation(reference, residual_ppm)),
        ("Measured FIR, synchronous", filtered),
        ("Measured FIR plus residual clock", clock_warp_interpolation(filtered, residual_ppm)),
        ("Measured SIP capture, clock corrected", clock_warp_interpolation(captured, -float(bench["clock_ppm"]))),
        ("Measured SIP capture", captured),
    ]

    simulations: dict[str, dict[str, np.ndarray | int | float]] = {}
    summary_rows: list[list[object]] = []
    block_rows: list[list[object]] = []
    for name, observed in cases:
        simulation = simulate_v90_equalizer_core(reference, observed)
        simulations[name] = simulation
        ave_pdsnr = float(simulation["ave_pdsnr"])
        summary_rows.append(
            [name, int(simulation["delay"]), ave_pdsnr, TRN1D_ERROR_THRESHOLD, ave_pdsnr > TRN1D_ERROR_THRESHOLD]
        )
        for symbol_index, rms_error, filtered_error in zip(
            np.asarray(simulation["symbol"]),
            np.asarray(simulation["rms"]),
            np.maximum(np.asarray(simulation["filtered"]), 1.0e-4),
        ):
            block_rows.append([name, int(symbol_index), rms_error, filtered_error])

    simulated_values = {name: float(simulation["ave_pdsnr"]) for name, simulation in simulations.items()}
    digital_synthetic = simulations["Digital, synchronous"]
    digital_measured = simulations["Measured SIP digital capture"]
    if not np.array_equal(
        np.asarray(digital_synthetic["filtered"]),
        np.asarray(digital_measured["filtered"]),
    ):
        raise AssertionError(
            "The aligned synthetic and measured digital-SIP trajectories must coincide"
        )
    if not simulated_values["Digital, synchronous"] < TRN1D_ERROR_THRESHOLD:
        raise AssertionError("The digital equalizer simulation must converge")
    if not simulated_values["Measured SIP digital capture"] < TRN1D_ERROR_THRESHOLD:
        raise AssertionError("The measured digital SIP equalizer simulation must converge")
    if not simulated_values["Measured FIR, synchronous"] < TRN1D_ERROR_THRESHOLD:
        raise AssertionError("The synchronous measured-FIR counterfactual must remain below threshold")
    if not simulated_values["Measured FIR plus residual clock"] > TRN1D_ERROR_THRESHOLD:
        raise AssertionError("The combined measured impairments must fail")
    if not simulated_values["Measured SIP capture"] > TRN1D_ERROR_THRESHOLD:
        raise AssertionError("The captured SIP input must fail")
    if not simulated_values["Measured SIP capture, clock corrected"] > TRN1D_ERROR_THRESHOLD:
        raise AssertionError("Clock correction alone must not repair the captured SIP input")

    write_csv(
        results / "ave_pdsnr_results.csv",
        ["case", "coarse_delay_samples", "final_1600_symbol_ave_pdsnr", "threshold", "fallback"],
        summary_rows,
    )
    write_csv(
        results / "equalizer_metric_blocks.csv",
        ["case", "symbol", "block_rms_error", "filtered_mean_error"],
        block_rows,
    )

    traces = bench_error_traces(data)
    trace_rows: list[list[object]] = []
    for name, (times, values) in traces.items():
        trace_rows.extend([[name, time, value] for time, value in zip(times, values)])
    write_csv(
        results / "bench_error_trace.csv",
        ["path", "seconds_after_trn1d_dd", "logged_filtered_mean_error"],
        trace_rows,
    )

    fig, ax = plt.subplots(figsize=(7.2, 4.0), constrained_layout=True)
    for name, (times, values) in traces.items():
        color = {
            "E1 digital": "#1f77b4",
            "SIP digital": "#2ca02c",
        }.get(name, "#d62728")
        display_name = {
            "SIP D/A-A/D, severe bypassed": "SIP D/A–A/D, fallback mascarado",
        }.get(name, name)
        ax.plot(
            times,
            values,
            marker="o",
            markersize=2.5,
            linewidth=1.2,
            label=display_name,
            color=color,
        )
    ax.axhline(TRN1D_ERROR_THRESHOLD, color="black", linestyle="--", label="limiar de fallback = 180")
    ax.set(
        yscale="log",
        xlabel="Tempo após a entrada no modo dirigido por decisão de TRN1d (s)",
        ylabel="Erro RMS filtrado registrado (unidades PCM)",
        title="Ensaios controlados: ambos os caminhos digitais convergem",
    )
    ax.legend(fontsize=8)
    save_figure(fig, figures, "bench_error_convergence")

    plotted = [
        ("Digital, synchronous", "Digital: sintético = SIP medido"),
        ("Measured FIR, synchronous", "FIR medido, síncrono"),
        ("Measured FIR plus residual clock", "FIR medido + relógio residual"),
        ("Measured SIP capture", "Captura SIP medida"),
    ]
    colors = ["#1f77b4", "#ff7f0e", "#9467bd", "#d62728"]
    fig, ax = plt.subplots(figsize=(7.2, 4.0), constrained_layout=True)
    for (name, display_name), color in zip(plotted, colors):
        simulation = simulations[name]
        ax.plot(
            np.asarray(simulation["symbol"]) / 8000.0,
            np.asarray(simulation["filtered"]),
            label=f"{display_name} ({float(simulation['ave_pdsnr']):.1f})",
            color=color,
        )
    ax.axhline(TRN1D_ERROR_THRESHOLD, color="black", linestyle="--", label="limiar de fallback = 180")
    ax.set(
        yscale="log",
        xlabel="Tempo simulado de TRN1d (s)",
        ylabel="Métrica RMS filtrada exata (unidades PCM)",
        title="Simulação do receptor; a legenda mostra a avePdsnr final",
    )
    ax.legend(fontsize=7.5, ncols=2)
    save_figure(fig, figures, "ave_pdsnr_convergence")

    return {
        **simulated_values,
        "residual_ppm": residual_ppm,
        "actual_sip_ave_pdsnr": 359.076,
    }


def factorial_simulation(
    bench: dict[str, object], results: Path, figures: Path
) -> None:
    source = 3904.0 * trn1d(80000)
    model_scores = np.asarray(bench["model_scores"])
    models = np.asarray(bench["models"])
    representative = models[np.argsort(model_scores)[len(model_scores) // 2]]
    channel_output = signal.lfilter(representative, [1.0], source)
    digital_model_scores = np.asarray(bench["digital_model_scores"])
    digital_models = np.asarray(bench["digital_models"])
    digital_representative = digital_models[
        np.argsort(digital_model_scores)[len(digital_model_scores) // 2]
    ]
    digital_channel_output = signal.lfilter(digital_representative, [1.0], source)
    clock_ppm = float(bench["clock_ppm"])
    cases = [
        ("Digital, synchronous", source),
        ("Measured digital SIP filter", digital_channel_output),
        ("Digital, measured clock offset", time_warp(source, clock_ppm)),
        ("Measured filter, synchronous", channel_output),
        ("Measured filter and clock offset", time_warp(channel_output, clock_ppm)),
    ]
    rows: list[list[object]] = []
    drops_by_case: list[np.ndarray] = []
    for name, samples in cases:
        scaled = samples * (12000.0 / (np.max(np.abs(samples)) + 1.0e-12))
        converted = rcfixed_8_to_9_6(scaled, fixedrc)
        start = 6 * FS_DSP
        _, power = dsplibs_psd(converted[start : start + PSD_LENGTH])
        values = nearest_bin_values(power)
        drops, severe = verifier_result(values)
        drops_by_case.append(drops)
        rows.append([name, *drops, severe])
    decisions = [bool(row[-1]) for row in rows]
    if decisions != [False, False, False, True, True]:
        raise AssertionError(f"Unexpected factorial decisions: {decisions}")
    write_csv(
        results / "factorial_results.csv",
        ["case", "drop_4000_db", "drop_4200_db", "severe"],
        rows,
    )
    fig, ax = plt.subplots(figsize=(8.0, 4.2), constrained_layout=True)
    positions = np.arange(len(cases))
    width = 0.34
    drops_array = np.asarray(drops_by_case)
    ax.bar(positions - width / 2, drops_array[:, 0], width, label="3600→4000 Hz")
    ax.bar(positions + width / 2, drops_array[:, 1], width, label="3600→4200 Hz")
    ax.axhline(SEVERE_THRESHOLD_DB, color="black", linestyle="--", label="Limiar de 28 dB")
    ax.set_xticks(
        positions,
        [
            "Digital\nsíncrono",
            "Filtro SIP\ndigital medido",
            "Digital\n+ desvio de relógio",
            "Filtro D/A–A/D\nsíncrono",
            "Filtro D/A–A/D\n+ desvio de relógio",
        ],
    )
    ax.set(
        ylabel="Queda de potência (dB)",
        title="Simulação fatorial: o filtro muda a decisão; o relógio isolado não",
    )
    ax.legend(ncols=3, fontsize=8)
    save_figure(fig, figures, "filter_clock_factorial")


def write_summary(
    results: Path, bench: dict[str, object], ave_pdsnr: dict[str, float]
) -> None:
    e1_values = np.asarray(bench["e1_values"])
    sip_values = np.asarray(bench["sip_values"])
    sip_digital_values = np.asarray(bench["sip_digital_values"])
    e1_drops = np.asarray(bench["e1_drops"])
    sip_drops = np.asarray(bench["sip_drops"])
    sip_digital_drops = np.asarray(bench["sip_digital_drops"])
    scores = np.asarray(bench["model_scores"])
    rc_points = np.asarray(bench["rc_points"])
    lines = [
        "Reproducible numerical summary",
        "==============================",
        "",
        f"E1 verifier powers at 3600/4000/4200 Hz: {e1_values[0]:.6f}, {e1_values[1]:.6f}, {e1_values[2]:.6f} dB",
        f"E1 drops: {e1_drops[0]:.6f}, {e1_drops[1]:.6f} dB; severe = {bool(np.all(e1_drops > SEVERE_THRESHOLD_DB))}",
        f"Digital SIP verifier powers at 3600/4000/4200 Hz: {sip_digital_values[0]:.6f}, {sip_digital_values[1]:.6f}, {sip_digital_values[2]:.6f} dB",
        f"Digital SIP drops: {sip_digital_drops[0]:.6f}, {sip_digital_drops[1]:.6f} dB; severe = {bool(np.all(sip_digital_drops > SEVERE_THRESHOLD_DB))}",
        f"D/A-A/D SIP verifier powers at 3600/4000/4200 Hz: {sip_values[0]:.6f}, {sip_values[1]:.6f}, {sip_values[2]:.6f} dB",
        f"D/A-A/D SIP drops: {sip_drops[0]:.6f}, {sip_drops[1]:.6f} dB; severe = {bool(np.all(sip_drops > SEVERE_THRESHOLD_DB))}",
        f"TRN1d generator offset: {bench['trn_offset']} symbols; normalized correlation: {bench['trn_correlation']:.9f}",
        f"Digital SIP TRN1d offset: {bench['sip_digital_trn_offset']} symbols; normalized correlation after polarity normalization: {bench['sip_digital_trn_correlation']:.9f}",
        f"Measured clock offset: {bench['clock_ppm']:.3f} ppm; delay-fit RMSE: {bench['clock_rmse']:.6f} sample",
        f"Digital SIP clock offset relative to E1: {bench['sip_digital_clock_ppm']:.6f} ppm; delay-fit RMSE: {bench['sip_digital_clock_rmse']:.9f} sample",
        f"Local 80-tap FIR validation R^2: median {np.median(scores):.6f}, range {np.min(scores):.6f} to {np.max(scores):.6f}",
        f"Additional measured loss relative to 3600 Hz: {bench['loss_3800']:.3f} dB at 3800 Hz and {bench['loss_3996']:.3f} dB at 3996 Hz",
        f"Digital SIP loss relative to 3600 Hz: {bench['sip_digital_loss_3800']:.3f} dB at 3800 Hz and {bench['sip_digital_loss_3996']:.3f} dB at 3996 Hz",
        f"RcFixed prototype response relative to DC at 3600/4000/4200 Hz: {rc_points[0]:.3f}, {rc_points[1]:.3f}, {rc_points[2]:.3f} dB",
        "",
        "Exact avePdsnr receiver-core simulation (final 1600-symbol evaluation):",
        f"  Digital, synchronous: {ave_pdsnr['Digital, synchronous']:.3f} (threshold {TRN1D_ERROR_THRESHOLD:.1f})",
        f"  Measured SIP digital capture: {ave_pdsnr['Measured SIP digital capture']:.3f}",
        f"  Digital, residual clock only: {ave_pdsnr['Digital, residual clock only']:.3f}",
        f"  Measured FIR, synchronous: {ave_pdsnr['Measured FIR, synchronous']:.3f}",
        f"  Measured FIR plus residual clock: {ave_pdsnr['Measured FIR plus residual clock']:.3f}",
        f"  Measured SIP capture, clock corrected: {ave_pdsnr['Measured SIP capture, clock corrected']:.3f}",
        f"  Measured SIP capture: {ave_pdsnr['Measured SIP capture']:.3f}",
        f"  Controlled binary run, measured avePdsnr: {ave_pdsnr['actual_sip_ave_pdsnr']:.3f}",
        f"  Residual clock used after the modem's acquired estimate: {ave_pdsnr['residual_ppm']:.3f} ppm",
    ]
    (results / "summary.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> None:
    args = arguments()
    args.results.mkdir(parents=True, exist_ok=True)
    args.figures.mkdir(parents=True, exist_ok=True)
    configure_plots()
    bench = bench_analysis(args.data, args.results, args.figures)
    factorial_simulation(bench, args.results, args.figures)
    ave_pdsnr = ave_pdsnr_simulation(
        bench, args.data, args.results, args.figures
    )
    write_summary(args.results, bench, ave_pdsnr)
    print((args.results / "summary.txt").read_text(encoding="utf-8"), end="")


if __name__ == "__main__":
    main()
