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
| `topology4` | Direct SIP-to-E1 extension | slmodem → SIP/RTP → 2911 → E1 → AS5300 |

Topologies 1--3 are the three configurations proposed in the original
project plan. Topology 4 is a later extension that isolates the HT503 from
the SIP path while retaining the 2911 E1 gateway.
