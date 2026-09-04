# Capture provenance

All WAV files are mono, signed 16-bit PCM at 8000 sample/s. Each TRN1d
excerpt contains a one-second pre-roll, and the analysis window begins at
sample 8000. Every artifact needed for the new direct-SIP analysis is stored
in this directory; the analysis does not depend on an external temporary file.

| Checked-in excerpt | Source offset | Length | SHA-256 |
|---|---:|---:|---|
| `e1_trn1d_preroll.wav` | 178320 | 24000 | `d52e3d3c8317e6ce34d8edeaf900a664a52a0aa96280f7248b13f28c9e5ee796` |
| `sip_da_ad_trn1d_preroll.wav` | 180800 | 24000 | `e7c8e374f21d9419e135e36ef552e988e8f6548055e1c27efce7687d4cd40e95` |
| `sip_digital_trn1d_preroll.wav` | 269600 | 24000 | `64821a386e17714a91397fbb736867267db093c39bae3f75e41c3d43dd12a375` |

The retained hashes of the two historical full sources were:

```text
bbb2cad96160e32aafe3bd2558192d890abdca5bc0a94327c988af4e587ce396  codex-v90-e1-rx.wav
163e04185f53af2e17b530427783257554cd649e0c8e02dd8887c409e5b085f4  codex-v90-clock-rx.wav
```

The corresponding historical decoded logs had these hashes:

```text
4b5f92e0ba0e85b9a61dd7cad422a4ffed70daad1e1de5d3a6397aaa4d735658  codex-v90-e1.log.decoded
30c79f218339ce736597cd35debfcc1146af558a1a3fe0144342c3a0d32b9d62  codex-v90-clock.log.decoded
10044da82515527580ebe37fe7e1362ad905002e235fce6d2172b2b6d4a07137  codex-sip-v90-mask-severe.log.decoded
```

The third historical log is the controlled binary-patch test. The temporary
copy of `dsplibs.o` set `enableDrop2V34OnSevereCodec` to zero; the ordinary
object was restored and hash-checked after the run. The log records the
detector firing, the message `drop is masked, doing nothing`, continued error
energy near 330 to 360, fallback at the end of TRN1d with
`avePdsnr = +359.076`, and the final `NO CARRIER` result.

The direct-SIP experiment of 2026-09-03 is retained in full:

| Checked-in artifact | SHA-256 |
|---|---|
| `sip_digital_v90_rx_full.wav` | `6141c3f419e33194ef5095a56e704f8bc69f43281879a06ecf7b58cc51027ed2` |
| `sip_digital_v90_tx_full.wav` | `300f444d7d2cb786bd142d210c884a4ecd0b28ea218e53ddaf09a9aa834fce80` |
| `sip_digital_v90.log` | `56ca37d3b9bb46c087a753bbbf7bb8d3533ed7e77ee47bca45ab1a35ef0c4e16` |

That run used ephemeral local SIP and RTP ports, PCMA to the Cisco 2911,
`IODELAY=240`, and three RAS `show clock` probes. It negotiated 28.8 kbit/s
upstream and 56 kbit/s downstream; all three probes returned the command echo,
UTC clock line, and router prompt. The log records 6092 contiguous 80-sample
RTP packets and the successful DSP training attempt.

`analyze.py` contains literal transcriptions of the historical E1 and
bypassed-D/A--A/D `V90Demodulator: Error Energy` values. It parses the same
quantity directly from the checked-in direct-SIP log and writes all three
trajectories to `results/bench_error_trace.csv`. These printed values are the
binary's `FilteredMeanError`, not a power in decibels; the hashes above bind
the measurements to their artifacts.
