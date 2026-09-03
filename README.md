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
| [`docs/`](docs/) | Project summary, DAHDI/Wanpipe and PRI notes, and the technical report (`relatorio.typ`). |
| [`analise_v90_sip/`](analise_v90_sip/) | V.90 spectral analysis of the SIP D/A–A/D path. |

`re/` and `baresip/` are embedded upstream trees (`re` is a git submodule
dependency of `baresip`); `slmodem-*/` is the vendored slmodem source.

## Build

```sh
make -C tools        # builds the C bridges in tools/
```

See `docs/relatorio.typ` (§ Documentacao e reproducibilidade) for the full
toolchain and `dialbench/README.md` for the CLI.
