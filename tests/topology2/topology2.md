# Topology 2 — slmodem over SIP (RAS validation)

Validation of the slmodem softmodem as the client against the RAS (AS5300/MICA),
using the Grandstream HT503 ATA to bridge SIP/RTP to the analog FXS path.

Status: V.34 RAS validation completed; remaining protocol coverage is tracked
separately.

---

## 1. Overview

```
[Linux: slmodem_bridge ↔ rtp_bridge]        SIP UA, local id sip:12@10.42.0.1
        │  INVITE to sip:42@10.42.0.102     (destination number = 42, in SIP)
        ▼
[Grandstream HT503 ATA @ 10.42.0.102]       FXO dials 42 on the line
        │  analog
        ▼
[2911 FXS] → dial-peer 42 → PRI E1 → [AS5300 MICA]
```

The **slmodem** (softmodem, DSP in `dsplibs.o`) replaces the hardmodem as the
client. `slmodem_bridge` wraps it as 8 kHz s16le PCM stdin/stdout + a PTY;
`rtp_bridge` is the SIP user agent. **Dialing is entirely SIP**: the destination
number (42) travels in the SIP Request-URI, and the ATA physically dials it on
its FXO to the 2911. The slmodem only provides the modem training audio — it
does not generate dial DTMF.

**Validation target:** the slmodem training against the MICA (V.21→V.90) over
the digital E1. A slmodem→hardmodem call through the ATA is an intermediate
check, NOT topology-2 validation — the RAS must be the far end.

---

## 2. SIP identities & call routing

| Role | Value | Source (code) |
|---|---|---|
| Local SIP id (origin) | `sip:12@10.42.0.1` (display "12") | `rtp_bridge.c:692,700` — `from_uri`, `sipsess_connect(...)` |
| Peer / destination | `sip:42@10.42.0.102:5062` | `rtp_bridge.c:20` — `-p/--peer` (default `sip:11@...`) |
| ATA | Grandstream HT503 @ 10.42.0.102:5062 | — |
| RAS dial number | **42** → dial-peer 42 → PRI → MICA | 2911 config |

Notes:
- `rtp_bridge`'s **From** is `sip:12@<local_ip>` — **12 is the originating
  address**, not the destination. Confirmed: `rtp_bridge` sets the local id
  `sip:12@10.42.0.1` (the `dialbench` `slmodem_sip` caller launches it for the
  SIP leg), and `baresip_play.c` registers `<sip:12@10.42.0.1>`.
- The **peer URI is what you dial**. Default `sip:11@...` was for the
  slmodem→Conexant test; set it to **`sip:42@10.42.0.102`** to reach the RAS.
  (Verified by changing 11→42: the call to the MICA completes.)
- The slmodem's own AT dial command is a bare `ATD` (no number) — the
  destination is carried by SIP, not by the modem.

---

## 3. Hardware / network

| # | Device | Role | Notes |
|---|---|---|---|
| 1 | Cisco AS5300 | RAS (MICA) | same as Topology 1 |
| 2 | Cisco 2911 | Voice gateway | same as Topology 1 |
| 3 | Grandstream HT503 | VoIP↔analog ATA | 1×FXS + 1×FXO; 10.42.0.102 |
| 4 | Linux PC | slmodem + tooling | 10.42.0.1, SIP id 12 |

2911 extension map:
| Ext | Port | Endpoint |
|---|---|---|
| 11 | FXS 0/1/1 | Conexant hardmodem |
| 12 | FXS 0/1/2 | ATA analog line (extension 12) |
| 42 / 666 | PRI (0/0/0:15) | AS5300 MICA (the target) |

The ATA's FXO connects to a 2911 FXS port; the 2911's dial-peer for that port
is "12". When the ATA dials 42, the 2911 routes dial-peer 42 → PRI → MICA.

---

## 4. Software stack

| Component | Role |
|---|---|
| `slmodem_bridge.c` | Wraps one 32-bit slmodem: PCM stdin/stdout + PTY; resamples 8k↔9.6k (slmodem data pump runs at 9600 Hz); sends a bare originate `ATD` on audio-path-established |
| `rtp_bridge.c` | SIP UA + RTP (libre); local id `sip:12@<local_ip>`, dials `-p` peer URI; 8 kHz s16le PCM on stdin/stdout; requires PCMA |
| `dialbench` (`modem slmodem_sip`) | Orchestrates both processes, cross-connects PCM (`process.spawn_pump_pair`), echoes stderr (`[slm]`/`[rtp]`), and runs the end-to-end RAS data probe |
| `Makefile` | Builds `slmodem_bridge` (`-m32`, links slmodem objects + 32-bit libsamplerate) |

### Build

```sh
make slmodem_bridge
```
Requires 32-bit toolchain + `lib32-libsamplerate` (multilib) — see
`SLMODEM_NOTES.md` for the per-distro package list.

### Protocol forcing — `-M` modulation register (S32)

From `modem_dp_drivers` in `slmodem-2.9.11-20110321/modem/modem.c`:

| Protocol | `-M` value |
|---|---|
| V.21 | 21 |
| V.22 | 22 |
| V.22bis | 122 |
| V.32 | 32 |
| V.32bis | 132 |
| V.34 | 34 |
| **V.90** | **90** |
| V.92 | 92 |
| K56Flex | 56 |

Bridge default `-M` = **122 (V.22bis)** — pass an explicit `-M` per test.

---

## 5. ATA configuration used by the validated path

The complete audited FXO profile is in
[`configs/ht503/`](../../configs/ht503/README.md). The ATA FXO connects to the
2911 FXS port associated with dial-peer 12. It listens on SIP/UDP port 5062,
dials Request-URI number 42 on the FXO line, and allows only PCMA/G.711 with
10 ms packets. VAD and line echo cancellation are disabled.

- The path necessarily contains ATA DAC/ADC conversion and independent ATA
  sample timing. It is therefore distinct from Topology 3 (direct E1) and
  Topology 4 (direct SIP-to-E1).

---

## 6. Procedure

1. Ensure the 2911/AS5300 PRI link is up (as in Topology 1).
2. Orchestrated run:
   ```sh
   python3 -m dialbench modem slmodem_sip \
     --peer sip:42@10.42.0.102:5062 \
     --slmodem-mode orig -M 90
   ```
   (`-M 90` = V.90; swap per protocol table. The destination is the peer URI —
   no separate dial number is needed.)
3. Watch stderr: `[slm]`/`[rtp]` lines, `Dialing sip:42@...`, then the slmodem
   result code (`CONNECT ...` / `NO CARRIER`).
4. On the AS5300: `show caller`, `show modem`, `debug modem csm` (training).
5. Capture slmodem debug output (see §8).
6. Repeat for V.21→V.90 in order.

Manual FIFO run + PTY access: see `SLMODEM_NOTES.md`.

---

## 7. Results

All passing entries require the end-to-end RAS data criterion: command echo,
`UTC` response, and `Router>` prompt for every `show clock` probe. The V.90
entry is intentionally not a pass: bypassing the spectral verifier did not
make the modem converge through the ATA DAC/ADC path.

| Protocol | Rate (↓/↑) | Modulation | Errors | slmodem log | Notes |
|---|---|---|---|---|---|
| V.21 | 300/300 bit/s | V.21 | none observed | Consolidated regression | 3/3 RAS responses. |
| V.22 | 1,200/1,200 bit/s | V.22 | none observed | Consolidated regression | 3/3 RAS responses. |
| V.22bis | 2,400/2,400 bit/s | V.22bis | none observed | Consolidated regression | 3/3 RAS responses. |
| V.23 | 1,200/1,200 bit/s | V.23 | none observed | Consolidated regression | 3/3 RAS responses. |
| V.32bis | 14,400/14,400 bit/s | V.32bis | none observed | Consolidated regression | 36/36 RAS responses across twelve calls. |
| V.34 | 26.4/24.0 kbit/s (RX/TX) | V.34 | none observed | Consolidated regression | 3/3 RAS responses on 2026-09-03. |
| V.90 | No functional connection | V.90 | Phase-4 convergence failure | V.90 analysis records | No RAS data pass through the ATA DAC/ADC path. |

---

## 8. Debug-output retention

`dialbench` prefixes runtime modem and RTP diagnostics as `[slm]` and `[rtp]`.
Only logs copied into this directory are durable experiment artifacts. The
results matrix does not cite transient host paths.

---

## 9. Findings

The HT503 DAC/ADC segment is the material difference from Topology 4's
digital SIP-to-E1 path. V.34 carries verified RAS traffic through it at
24.0/26.4 kbit/s, whereas V.90 has no functional data pass on this path.

---

## 10. Artifacts

- `tests/topology2/results.csv` — consolidated matrix.
- `tests/topology2/t2-TEMPLATE.log` — per-run template (adds a slmodem-debug section).
- `docs/SLMODEM_NOTES.md` — integration and build notes.
