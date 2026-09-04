# Grandstream HT503

[`pertinent-settings.txt`](pertinent-settings.txt) records the FXO profile
parameters that affect topology 2. The values were checked against the active
NVRAM export; passwords, authentication identifiers, the MAC address, and
settings unrelated to the experimental path were excluded.

The HT503 uses DHCP and held `10.42.0.102` during the audit. Its FXO profile
listens for SIP/UDP on port 5062, starts RTP at port 5012, accepts calls without
registration, and dials the number carried by the SIP URI on the analog line.

## Parameter mapping

| Parameter | Value | Web interface field |
|---|---:|---|
| P8 | 0 | Obtain the address by DHCP. |
| P401 | 1 | `Account Active`: yes. |
| P731 | 0 | `SIP Registration`: no. |
| P813 | 1 | `Outgoing Call without Registration`: yes. |
| P747 | `10.42.0.1` | `Primary SIP Server`. |
| P830 | 0 | `SIP Transport`: UDP. |
| P730 | 0 | `NAT Traversal`: disabled on the local bench network. |
| P740 / P739 | 5062 / 5012 | Local SIP and RTP ports. |
| P778 | 0 | `Use Random Port`: no. |
| P757–P762, P814–P815 | 8 | All eight codec preferences set to PCMA. |
| P737 | 1 | One G.711 frame per packet, or 10 ms. |
| P750 | 0 | VAD disabled. |
| P460 | 1 | Symmetric RTP enabled. |
| P710 | 1 | Fax pass-through instead of T.38. |
| P831 / P832 | 0 / 0 | Fixed, short jitter buffer. |
| P248 / P283 | 0 / 0 | TX and RX gain at 0 dB. |
| P825 | 1 | Line echo canceller disabled. |
| P899 / P864 | 0 / 0 | Country-based electrical profile, USA. |
| P849 | 30 | Impedance selector, inactive while P899 is 0. |
| P3300 / P3302 | 100 / 100 | DTMF duration and pause in milliseconds. |
| P3303 / P3304 | 1 / 1 | Wait for dial tone and use one-stage dialing. |
| P3206 | 500 | Minimum 500 ms delay before PSTN dialing. |

## Restoration and verification

Restore the values on the web interface's **FXO Port** page, apply them, and
reboot the ATA. Do not copy credentials from another device. Then verify:

1. the DHCP lease;
2. SIP/UDP listening on port 5062;
3. exclusive PCMA negotiation;
4. routing `sip:42@<HT503-address>:5062` to number 42 on the FXO line;
5. disabled VAD and echo cancellation.
