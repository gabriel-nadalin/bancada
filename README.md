# dial-up-bench — slmodem reverse-engineering test bench

A dial-up test bench for reverse engineering the slmodem softmodem: a
userspace bridge between slmodem and SIP/RTP or E1 transports, an ISDN PRI
signalling tool, a Python orchestrator, and recorded results against a Cisco
AS5300/MICA RAS. The bench exercises V.21–V.90 and validates calls end-to-end
through the modem data path.

## Components

| Path | What it is |
|---|---|
| [`dialbench/`](dialbench/README.md) | Python orchestrator: audio-path latency and modem-call tests. **Start here for usage.** |
| [`tools/`](tools/) | C transports: `rtp_bridge`, `slmodem_bridge`, `baresip_play`, `pri_call`. |
| [`tests/`](tests/README.md) | Topology records (`topology1`–`topology4`): procedures, logs, result matrices. |
| [`docs/`](docs/) | Project summary, DAHDI/Wanpipe and PRI notes, and the ICTSR final report (`relatorio.tex`). |
| [`configs/`](configs/README.md) | Sanitized snapshots of the active Linux, Cisco 2911, AS5300, and HT503 configurations, with restoration checks. |
| [`analysis/v90_sip/`](analysis/v90_sip/README.md) | Reproducible V.90 spectral, clock, and equalizer analysis with the supporting captures and CSVs. |

`re/` and `baresip/` are Git submodules pinned to the revisions used in the
bench. The `slmodem` distribution is downloaded from the Debian archive,
verified by SHA-256 and patched with the single portability change present on
the bench host.

## Build

```sh
make dependencies    # initializes the submodules and prepares slmodem
make                 # builds the C bridges in tools/
make analysis-v90    # regenerates CSVs and the report figures
make report          # regenerates the analysis and builds the ICTSR report
```

See `docs/relatorio.tex` (§ Documentação e reprodutibilidade) for the full
toolchain and `dialbench/README.md` for the CLI.
