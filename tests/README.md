# Bench topology records

Each topology directory contains a `topologyN.md` description and a
`results.csv` matrix. Every matrix uses this schema:

```
topology,protocol,date,transport,peer,codec,result,tx_rate_bps,rx_rate_bps,ras_responses,notes
```

`ras_responses` records verified end-to-end commands through the modem data
path. `not-recorded` is used only for historical hardmodem logs that predate
the PTY-based RAS probe; it is not equivalent to a modern data-path pass.

| Directory | Topology | Path |
|---|---|---|
| `topology1` | Hardmodem validation | USB Conexant → analog FXS → 2911 → E1 → AS5300 |
| `topology2` | VoIP ATA integration | slmodem → SIP/RTP → HT503 → analog FXS → 2911 → E1 → AS5300 |
| `topology3` | Direct digital E1 | slmodem → Sangoma E1 → AS5300 |
| `topology4` | Direct SIP-to-E1 control | slmodem → SIP/RTP → 2911 → E1 → AS5300 |

Topology 4 isolates the HT503 while retaining the 2911 E1 gateway, making it
the control path for separating ATA conversion effects from SIP/RTP effects.
