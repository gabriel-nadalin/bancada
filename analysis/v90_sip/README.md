# V.90 analysis for the E1 and SIP paths

This directory contains the reproducible analysis used by the ICTSR final
report. It explains why the direct E1 path and the digital SIP-to-E1 path
preserve V.90 downstream training, while the path through the HT503 analog
interface does not.

## Organization

| Path | Content |
|---|---|
| `analyze.py` | Spectral verifier, channel and clock measurements, and equalizer simulations. |
| `fixedrc.py` | Reconstructed fixed-rate converter required for the exact 8-to-9.6 ksample/s path. |
| `data/` | Bench captures, the complete digital-SIP call, its log, provenance, and hashes. |
| `results/` | Regenerated CSV tables and the numerical summary. |
| `../../docs/figures/` | PDF figures generated directly for `docs/relatorio.tex`. |

The canonical account of the investigation is the ICTSR report in
`docs/relatorio.tex`.

## Reproduction

From the repository root:

```bash
python3 -m venv analysis/v90_sip/.venv
analysis/v90_sip/.venv/bin/pip install -r analysis/v90_sip/requirements.txt
analysis/v90_sip/.venv/bin/python analysis/v90_sip/analyze.py
```

The script is self-contained within this repository. By default it reads
`data/`, rewrites the generated CSVs and summary in `results/`, and writes the
PDF figures directly to `docs/figures/`. The three locations can be overridden
with `--data`, `--results`, and `--figures`.

The checked-in TRN1d WAV files are three-second excerpts from actual bench
captures. Each contains a one-second pre-roll, allowing the reconstructed
stateful rate converter to reach the exact state used at the start of the
4096-sample verifier window. See `data/README.md` for provenance and hashes.

The conclusions are based on five independent checks:

1. exact reproduction of the three `V90SpectralVerifier` bins from all three
   bench captures;
2. exact identification of both pure-digital waveforms as the V.90 TRN1d
   sequence emitted by the reconstructed 18/23 scrambler;
3. comparative system identification and clock-drift measurements for the
   digital SIP and SIP D/A--A/D paths;
4. a digital SIP bench call that negotiated 28.8/56 kbit/s and returned three
   valid RAS command responses; and
5. a controlled run in which the severe-codec fallback was disabled, after
   which training still failed on the equalizer-error criterion.

The receiver simulation reconstructs the 300-tap LE/12-tap DFE training core
and the exact `avePdsnr` recurrence. It evaluates the E1 waveform, the measured
digital-SIP capture, the independently measured D/A--A/D FIR, the residual
clock offset, and the actual D/A--A/D SIP capture. No modem implementation or
bench configuration is changed by the analysis.
