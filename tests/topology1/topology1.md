# Topology 1 — Hardmodem Validation

Validation of the RAS configuration and V.90 capability using the USB hardmodem
(Conexant CX93010) as a reference client. A hardware modem eliminates software
variables, isolating any problem to the Cisco equipment configuration.

Test date: 2026-08-27/28.

---

## 1. Overview

```
Computer ──USB──▶ Hardmodem CX93010 ──analog──▶ Cisco 2911 (FXS)
                                                    │ E1 0/0/0 (PRI)
                                                    ▼
                                              Cisco AS5300 (MICA)
```

The hardmodem dials through an FXS port of the Cisco 2911; the 2911 forwards the
call out its E1 trunk over ISDN PRI to the Cisco AS5300, where a MICA digital
modem answers and trains. The AS5300 sits on a digital E1 (PRI), which is what
makes V.90 downstream possible.

---

## 2. Hardware

| # | Device | Role | Notes |
|---|---|---|---|
| 1 | Cisco AS5300 | RAS, terminates dial-up with MICA modems | E1 PRI, `mica-modem-pw.2.9.5.0.bin` |
| 2 | Cisco 2911 | Voice gateway: FXS ↔ E1 | VWIC3-1MFT-T1/E1 + VIC3-4FXS + PVDM3 |
| 3 | USB hardmodem Lenovo (Conexant CX93010) | Reference client modem | /dev/ttyACM2 on the bench PC |
| 4 | Linux PC | Hosts the modem, tooling, logs | picocom, `testes/topology1/` |

Consoles on the bench PC: `ttyACM0` = Cisco USB console (2911), `ttyUSB0` =
USB-serial adapter (AS5300).

---

## 3. Network path

| Leg | Detail |
|---|---|
| Modem → 2911 | Analog, FXS port 0/1/1 (from call trace; confirm cabling) |
| 2911 → AS5300 | E1 0/0/0, ISDN PRI, `pri-group timeslots 1-10,16` |
| AS5300 side | E1 controller 1, `pri-group timeslots 1-31` (MICA slot 2) |

Dialed number: **42** (dial-peer 42 → `port 0/0/0:15`, the PRI D-channel).
Number 666 routes identically.

---

## 4. Cisco 2911 configuration

Restored from `backup_cisco2911.txt`. This is the working PRI config.

```text
version 15.1
hostname Router
isdn switch-type primary-net5
card type e1 0 0
!
controller E1 0/0/0
 clock source internal
 pri-group timeslots 1-10,16
!
interface Serial0/0/0:15
 isdn switch-type primary-net5
 isdn protocol-emulate network
 isdn incoming-voice voice
!
voice-port 0/0/0:15
 no echo-cancel enable
 no non-linear
 no vad
 cptone BR
!
voice-port 0/1/0 | 0/1/1 | 0/1/2 | 0/1/3      (FXS)
 no echo-cancel enable
 no non-linear
 no vad
 compand-type a-law
 cptone BR
!
dial-peer voice 42 pots
 destination-pattern 42
 port 0/0/0:15
!
dial-peer voice 666 pots
 destination-pattern 666
 port 0/0/0:15
!
dial-peer voice 10/11/12/13 pots   (FXS 0/1/0..0/1/3)
```

Notes:
- `:15` in `0/0/0:15` is the PRI **D-channel** (E1 timeslot 16, 0-indexed),
  not a second E1. The physical trunk is `controller E1 0/0/0`.
- `pri-group timeslots 1-10,16` (10 B + D) is chosen because the 2911's PVDM3
  DSP has only ~12 free timeslots at the configured codec complexity; a full
  `1-31` PRI requires more DSP (see §9).

---

## 5. Cisco AS5300 configuration (relevant)

```text
controller E1 1
 pri-group timeslots 1-31        ! the E1 trunk from the 2911 (MICA slot 2)
isdn incoming-voice modem
!
async mode interactive
modem InOut
modem country mica e1-default
```
Firmware: `flash:mica-modem-pw.2.9.5.0.bin`.

---

## 6. PC-side setup

```sh
sudo picocom -b 115200 /dev/ttyACM2     # the Conexant hardmodem
```

Modem init (verbose + extended/protocol result codes):

```
AT&FE1V1Q0X4W2
```

---

## 7. Validation procedure

For each protocol, in order V.21 → V.22 → V.22bis → V.32bis → V.34 → V.90:

1. Force the modulation (disables auto-negotiation, no fallback):
   ```
   AT+MS=V21,0     ! V.21   (300)
   AT+MS=V22,0     ! V.22   (1200)
   AT+MS=V22B,0    ! V.22bis (2400)
   AT+MS=V32B,0    ! V.32bis (14400)
   AT+MS=V34,0     ! V.34   (33600)
   AT+MS=V90,0     ! V.90   (downstream up to 56000)
   ```
2. Dial: `ATD42`
3. Expect `CONNECT <rate>`. Capture `at&v1` (last-call stats) for the
   authoritative negotiated rate/protocol/retransmit counts.
4. Hang up: `+++` (wait 1–2 s) then `ATH0`.
5. On the AS5300, capture `show caller`, `show modem`, and (during a call)
   `debug modem csm` for the training sequence.

Reset auto-negotiation between protocols: `AT&F`.

---

## 8. Results

| Protocol | Rate (↓/↑) | Modulation | Errors | MICA | File |
|---|---|---|---|---|---|
| V.21 | 300 | FSK | none | 2/5 | `t1-v21-2026-08-27.log` |
| V.22 | 1200 | PSK/DPSK | none | 2/7 | `t1-v22-2026-08-27.log` |
| V.22bis | 2400 | QAM | none | 2/9 | `t1-v22bis-2026-08-27.log` |
| V.32bis | 14400 | QAM-128+trellis | none | 2/10 | `t1-v32bis-2026-08-27.log` |
| V.34 | 33600/31200 | QAM-1664+precoding | none | 2/11 | `t1-v34-2026-08-27.log` |
| V.90 | 46667/31200 | PCM↓ / QAM↑ | none | 2/12 | `t1-v90-2026-08-27.log` |

Consolidated in `resultados.csv`. 6/6 success, zero retrains, 100% MICA success
rate across all calls.

---

## 9. Findings / issues encountered

- **E&M vs PRI mismatch (root cause of initial `NO CARRIER`).** The 2911's E1
  was previously configured as an E&M voice-port (`voice-port 0/0/0:1`); the
  call trace showed `em_send_digits nothing to dial!!` — E&M CAS carries no
  called-number, and the AS5300 (PRI) never received a SETUP. Fixed by
  restoring the PRI config (`backup_cisco2911.txt`). Both ends of an E1 must
  use the same signaling.
- **DSP resource limit on the 2911.** A full `pri-group timeslots 1-31`
  was rejected ("Not enough DSP resources... enough for 12 time slots").
  `timeslots 1-10,16` fits and matches the AS5300's smaller group.
- **Line quality scales down with speed (expected).** `Line QUALITY`/`Rx LEVEL`
  dropped from 127/041 (V.22) to 002/014 (V.32bis), 017/017 (V.34), 042/013
  (V.90) — the higher-modulation protocols require more SNR margin, yet all
  held with zero retransmits. This is the analog FXS path's noise budget.
- **V.34/V.90 are asymmetric by design** (RX ≠ TX). The higher downstream rate
  is normal line-probing behavior, not a defect.
- **`TERMINATION REASON: CARRIER LOSS`** appears in some `at&v1` captures even
  on intentional `+++/ATH` teardown — benign; not a call failure.

---

## 10. Artifacts

- `backup_cisco2911.txt` — the working 2911 PRI config (restore point).
- `tests/topology1/*.log` — per-protocol run logs (modem session + AS5300
  `debug modem csm` + `show modem`, verbatim).
- `tests/topology1/results.csv` — consolidated connectivity matrix.
- `tests/topology1/t1-TEMPLATE.log` — blank template for future runs.

Open items: confirm exact FXS port (trace shows 0/1/1) and AS5300 E1 number
(inferred E1 1 from MICA slot 2).
