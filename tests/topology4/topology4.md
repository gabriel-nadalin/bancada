# Topology 4 — Direct SIP-to-E1 RAS Validation

This post-project extension removes the HT503 and its analog FXS/FXO segment
from the slmodem-to-RAS call path. It is intended to distinguish behavior
caused by the analog ATA conversion from behavior of the slmodem, SIP/RTP
transport, or the AS5300.

## Path

```
[slmodem_bridge + rtp_bridge, 10.42.0.1]
    -- SIP / RTP PCMA (8 kHz) -->
[Cisco 2911, 10.42.0.22]
    -- PRI E1 -->
[Cisco AS5300 / MICA RAS]
```

The 2911 accepts `sip:42@10.42.0.22:5060` and routes extension `42` to its
existing PRI dial-peer. RTP uses PCMA (G.711 A-law) at 8 kHz. Unlike Topology
2, there is no ATA DAC, analog line, or ATA ADC in either modem-audio
direction.

## Cisco 2911 changes

The following additions are persisted in the 2911 startup configuration:

```
interface GigabitEthernet0/0
 ip address dhcp
 no shutdown
!
voice service voip
 ip address trusted list
  ipv4 10.42.0.1
 sip
  bind control source-interface GigabitEthernet0/0
  bind media source-interface GigabitEthernet0/0
!
dial-peer voice 142 voip
 incoming called-number 42
 session protocol sipv2
 codec g711alaw
 dtmf-relay rtp-nte
 no vad
```

The trusted-address entry is deliberately restricted to the bench host. It is
required because IOS toll-fraud protection otherwise rejects the SIP INVITE
with Q.850 cause 21 before outbound PRI dial-peer selection.

## Test isolation

`rtp_bridge` accepts independent local SIP and RTP ports. Each bench call uses
a fresh pair so that SIP dialog state and media sockets are not shared with
another topology or a preceding call:

```
python3 -m dialbench modem slmodem_sip \
  --peer 'sip:42@10.42.0.22:5060;transport=udp' \
  --sip-port 5064 --rtp-port 4008 ...
```

Both default ports are ephemeral (`0`). Existing callers can still request
fixed signaling and media ports explicitly.

## Validity criterion

A carrier indication alone is not a passing modem test. After `CONNECT`,
`dialbench` opens the slmodem PTY and sends `show clock` to the AS5300. Each
passing exchange must contain the command echo, a line containing `UTC`, and
the `Router>` prompt. Three complete exchanges are required.

## Results

| Protocol | Result | Negotiated rate | RAS data probe | Notes |
|---|---|---:|---:|---|
| V.21 | Pass | 300/300 bit/s | 3/3 | Clean call using SIP/RTP ports 5066/4012. |
| V.22 | Pass | 1,200/1,200 bit/s | 3/3 | Clean call using SIP/RTP ports 5069/4018. |
| V.22bis | Pass | 2,400/2,400 bit/s | 3/3 | Clean call using SIP/RTP ports 5070/4020. |
| V.23 | Pass | 1,200/1,200 bit/s | 3/3 | Clean call using SIP/RTP ports 5068/4016 and a 10-second post-connect settle interval. |
| V.32bis | Pass | 14,400/14,400 bit/s | 3/3 | Clean call using SIP/RTP ports 5071/4022; native 8 kHz/`IODELAY=0`. |
| V.34 | Pass | 31,200/33,600 bit/s | 3/3 | Latest serial regression used ephemeral SIP/RTP ports on 2026-09-03. |
| V.90 | Pass | 26,400--28,800/54,667--56,000 bit/s | 3/3 in each of four calls | Four clean calls passed; the latest used ephemeral SIP/RTP ports with `IODELAY=240`. |

### V.21 run

Run date: 2026-09-03. This clean repetition used SIP/RTP ports 5066/4012.

```
python3 -m dialbench modem slmodem_sip \
  --peer 'sip:42@10.42.0.22:5060;transport=udp' \
  --sip-port 5066 --rtp-port 4012 \
  -M 21 --probe-count 3 --probe-max-attempts 3 \
  --probe-connect-timeout 50 --probe-response-timeout 25
```

The direct SIP leg reached `180 Ringing`, RTP was established with PCMA, and
the slmodem negotiated V.21 at 300 bit/s in both directions. The RAS replies
were:

```
Router>show clock
*11:23:28.235 UTC Thu Sep 3 2026
Router>

Router>show clock
*11:23:36.063 UTC Thu Sep 3 2026
Router>

Router>show clock
*11:23:43.635 UTC Thu Sep 3 2026
Router>
```

### V.90 IODELAY=240 control run

Run date: 2026-09-03. A serial control run used ephemeral SIP and RTP ports,
the native 9.6 kHz data-pump rate, and `IODELAY=240`.

```
python3 -m dialbench modem slmodem_sip \
  --peer 'sip:42@10.42.0.22:5060;transport=udp' \
  --sip-port 0 --rtp-port 0 -M 90 --debug-level 2 \
  --io-delay 240 --modem-rate 9600 \
  --probe-count 3 --probe-max-attempts 3 \
  --probe-connect-timeout 50 --probe-response-timeout 25
```

The modem negotiated 28,800 bit/s upstream and 56,000 bit/s downstream, then
completed all three end-to-end RAS exchanges. The VPCM requested one phase-4
retrain before `CONNECT`; the request recovered successfully and therefore is
not itself a terminal failure. No `I/O delay adjusted` event occurred. RTP RX
contained 6,092 PCMA packets of 80 samples with contiguous timestamps. RTP TX
intervals were 6--12 ms; this is packet-arrival variation, not evidence of a
deadline miss, because the call completed all three data probes.
