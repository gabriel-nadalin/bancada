# dialbench

Python orchestrator for the dial-up bench: drives audio-path **latency**
measurements and **modem-call** tests across the RTP/SIP and E1 transports.
Run it as a module:

```sh
python3 -m dialbench --help
```

`dialbench` does not ship the modem or the C transports. It launches and wires
together the compiled tools under `../tools` (`rtp_bridge`,
`slmodem_bridge`, `pri_call`, `baresip_play`) and, for `pjsua`, an external
UA installed on the host. Build the C tools first:

```sh
make -C tools
```

The E1 path (`slmodem_e1`) also needs `pri_call` and root access to the DAHDI
device; the caller prefixes its command with `sudo`.

## Layout

| Module | Responsibility |
|---|---|
| `cli.py` | `argparse` entry: subcommands `gen`, `analyze`, `latency`, `modem`. |
| `signal.py` | TX burst-signal generation (`generate_tx_wav`). Pure DSP, no I/O. |
| `analysis.py` | RX burst/latency analysis (`analyze_pair`). Pure DSP, no I/O. |
| `callers/` | One class per transport; registry of latency vs modem callers. |
| `process.py` | Process orchestration: cross-pumps two processes' stdio (`spawn_pump_pair`), the end-to-end PTY data probe (`PtyDataProbe`), teardown (`wait_cleanup`). |
| `paths.py` | DSP constants and repo paths (source of truth for `TOOLS_DIR`, `DATA_DIR`). |

`signal.py` and `analysis.py` are standalone DSP (no call orchestration), so
they can be exercised in isolation or reused by other tooling. `cli.py` is the
only module that touches the network of caller flags.

## Commands

### `gen`

Create a TX burst WAV. Usage is `dialbench gen <tx_wav>`.

Writes a mono s16/8 kHz WAV: sine bursts (default 1000 Hz, 100 ms on) every
500 ms, half-scale amplitude, first burst at `t = 0`. Options via `--freq`,
`--burst`, `--period`, `--dur`.

### `analyze`

Compare a TX and RX WAV and report delays. Usage is
`dialbench analyze <tx_wav> <rx_wav> [--csv FILE]`.

### `latency`

Full latency benchmark: ensure the TX WAV exists, establish a call through a
caller, play the stimulus, capture the returning media, then analyze.

```sh
python3 -m dialbench latency <caller> [--tx-wav FILE] [--rx-wav FILE] \
    [--peer URI] [--freq 1000] [--burst 100] [--period 500] \
    [--dur 10] [--ptime 10]
```

`<caller>` is one of the latency callers (`baresip`, `pjsua`, `rtp_bridge`).
Default RX paths land in `audios/` (see [Outputs](#outputs)).

### `modem`

Establish a modem call through a caller and observe training end-to-end. Usage
is `dialbench modem <caller> [--peer URI] [caller flags]`.

`<caller>` is one of the modem callers (`slmodem_sip`, `slmodem_e1`). The
caller originates the modem (slmodem) and pumps it against the RTP/SIP or E1
transport. This is not a latency measurement: it exercises the training path
and, by default, requires `--probe-count` successful end-to-end data probes
after `CONNECT`.

Modulation is selected with `-M` (`SREG_DP` value): `21`=V.21, `22`=V.22,
`23`=V.23, `122`=V.22bis, `132`=V.32bis, `34`=V.34, `90`=V.90. The default is
`122` for `slmodem_sip` and `90` for `slmodem_e1`.

## Callers

A caller is one way to establish a call path. A caller is a *latency* caller
if it subclasses `LatencyCaller` (it implements `run_latency`) and a *modem*
caller if it subclasses `ModemCaller` (it implements `run_modem`); a transport
may support both. The registry and split live in `callers/__init__.py`.

| Caller | Transport | Kind | C tool / runtime | Default SIP peer |
|---|---|---|---|---|
| `baresip` | SIP/RTP | latency | `tools/baresip_play` | `sip:11@10.42.0.102:5062;transport=udp` |
| `rtp_bridge` | SIP/RTP | latency | `tools/rtp_bridge` (PCMA) | `sip:11@10.42.0.102:5062;transport=udp` |
| `pjsua` | SIP/RTP | latency | `pjsua` (external) | `sip:11@10.42.0.102:5062;transport=udp` |
| `slmodem_sip` | SIP/RTP | modem | `slmodem_bridge` + `rtp_bridge` | `sip:42@10.42.0.102:5062;transport=udp` |
| `slmodem_e1` | E1 (Sangoma) | modem | `slmodem_bridge` + `pri_call` (sudo) | — |

The three latency callers dial the same default peer, so a latency comparison
keeps the far end fixed and isolates the client-side RTP/SIP implementation.

## Signal and measurement model

- Sample rate is 8 kHz, mono s16 (`SRATE`), half-scale sine amplitude
  (`AMP = 0.5 * 32767`).
- `signal.generate_tx_wav` writes a burst pattern: on/off cadence derived from
  `--burst`/`--period`, first burst at 0 ms.
- `analysis` validates both WAVs as mono s16/8 kHz and detects the burst
  cadence by cross-correlating the Goertzel envelope of the tone (10 ms
  windows) with an on/off pattern derived from the same `--burst`/`--period`
  used to generate the signal, so TX and RX are always analyzed with the
  cadence `gen` actually wrote. Both must fit whole 10 ms windows and leave at
  least one silence window (`cadence_windows` rejects otherwise). It reports:
  - **pipeline delay** — cross-correlation of the TX/RX Goertzel envelopes,
    search bounded to `max(500, 2 * period)` ms; a positive value means RX is
    delayed relative to TX;
  - **per-burst pairing** — each TX burst is paired to the nearest RX burst at
    `tx + delay ± period/3`, with missed bursts counted; printed per burst as
    `tx_ms`, `rx_ms`, `delay_ms`, plus mean/min/max/stdev over paired bursts.
- Exit code is `0` when every TX burst is paired and `2` when any are missing
  (a >25% loss also prints a warning to stderr). `analyze --csv` writes the
  per-burst rows (`burst,tx_ms,rx_ms,delay_ms`) to a file.

The detection is cadence-matched to cancel a constant background (e.g.
ringback harmonic leakage at the target frequency); it reports only burst
energy above the local average.

## Outputs

| File | Producer | Purpose |
|---|---|---|
| `audios/bench_tx.wav` | `gen` / `latency` (auto) | shared TX stimulus (default `--tx-wav`) |
| `audios/baresip_rx.wav` | `latency baresip` | captured RX |
| `audios/bridge_rx.wav` | `latency rtp_bridge` | captured RX |
| `audios/pjsua_rx.wav` | `latency pjsua` | captured RX |
| `--csv FILE` | `analyze` | per-burst delay rows |

All WAVs are transient runtime artifacts (ignored via `*.wav`); they are not
durable experiment records.

## Data probe (modem validation)

The modem callers require end-to-end data traffic before a pass. After the
modem reports `CONNECT`, `PtyDataProbe` opens the slmodem PTY and sends a
command through the data path, requiring the command echo, the expected
response, and the prompt to return before counting a response. Tunables:
`--probe-command`, `--probe-expect`, `--probe-prompt`, `--probe-count`,
`--probe-max-attempts`, `--probe-interval`, `--probe-settle`,
`--probe-connect-timeout`, `--probe-response-timeout`. `--debug-level` must be
at least `1` so the probe can observe `CONNECT`.

Native-rate and reported I/O-delay overrides (`--modem-rate`, `--io-delay`,
`--max-rate`, `--v32bis-retrain-snr`) are shared by the modem callers. The
defaults and the reasoning behind them are documented in
`../docs/SLMODEM_NOTES.md`; keep that file authoritative rather than restating
the profiles here.

## See also

- `../docs/SLMODEM_NOTES.md` — slmodem/E1/SIP integration notes: modem timing
  profiles, IODELAY meaning, and the end-to-end data criterion.
- `../tests/topology1/` … `topology4/` — per-topology procedures and the exact
  reproducible `modem` invocations used for each recorded result.
- `../docs/relatorio.typ` — the technical report (Portuguese), including the
  latency measurement methodology.
- `../tools/` — the C transports (`rtp_bridge`, `slmodem_bridge`,
  `baresip_play`, `pri_call`) that `dialbench` launches.
