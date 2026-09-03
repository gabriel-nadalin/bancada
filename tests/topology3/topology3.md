# Topology 3 — Direct E1 RAS Validation

This is the direct-digital topology proposed in the original project plan:
the bench PC connects through its Sangoma E1 interface directly to the Cisco
AS5300. It has no Cisco 2911, HT503, SIP/RTP, DAC, or ADC in the modem-audio
path.

## Path

```
[slmodem_bridge + pri_call, bench PC]
    -- E1 CCS/HDB3/CRC4 -->
[Cisco AS5300 / MICA RAS]
```

`pri_call` originates the EuroISDN call on a DAHDI B-channel and bridges its
A-law samples to `slmodem_bridge`. The Sangoma interface recovers the E1 clock
from the AS5300. Thus, after the A-law quantization at the bridge boundary,
the audio remains in one synchronous digital timing domain up to the RAS.

## Validity criterion

A result is a pass only after `CONNECT` and complete bidirectional data
traffic. `dialbench` sends `show clock` through the slmodem PTY and requires
the command echo, a line containing `UTC`, and the `Router>` prompt. The
default is three successful responses; a carrier indication alone is not a
pass.

## Consolidated results

The following direct-E1 results were previously recorded in
`docs/SLMODEM_NOTES.md`; this table is their topology-specific home.

| Protocol | Result | Negotiated TX/RX rate | RAS data probe | Notes |
|---|---|---:|---:|---|
| V.21 | Pass | 300/300 bit/s | 3/3 | Native 9.6 kHz profile, `IODELAY=240`. |
| V.22 | Pass | 1,200/1,200 bit/s | 3/3 | Native 9.6 kHz profile, `IODELAY=240`. |
| V.22bis | Pass | 2,400/2,400 bit/s | 3/3 | Native 9.6 kHz profile, `IODELAY=240`. |
| V.23 | Pass | 1,200/1,200 bit/s | 3/3 | Native 9.6 kHz profile, `IODELAY=240`. |
| V.32bis | Pass | 14,400/14,400 bit/s | 12/12 across four calls | Native 8 kHz profile, `IODELAY=0`. |
| V.34 | Pass | 33,600/33,600 bit/s | 3/3 | Repeated serial regression on 2026-09-03. |
| V.90 | Pass | 31,200/56,000 bit/s | 5/5 | Native 9.6 kHz profile, `IODELAY=240`. |

### V.34 serial regression, September 3, 2026

```
python3 -m dialbench modem slmodem_e1 \
  -M 34 --debug-level 1 \
  --probe-count 3 --probe-max-attempts 3 \
  --probe-connect-timeout 55 --probe-response-timeout 25
```

The modem negotiated 33,600 bit/s in each direction and completed all three
`show clock` exchanges. This run used the standard VPCM profile at 9.6 kHz
with `IODELAY=240`.
