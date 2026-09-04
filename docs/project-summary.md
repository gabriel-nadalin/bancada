# Summary — Dial-Up Test Bench for slmodem Reverse Engineering

## Project Goal

Build a real dial-up test bench using a **Cisco AS5300** (RAS with MICA digital modems and E1 interfaces) to validate the reconstruction of the `dsplibs.o` binary from the *slmodem* — a Linux softmodem driver whose DSP module was never released as source code. The bench enables real connections, especially **V.90** (56 kbps downstream), which requires a digital E1 trunk on the server side — impossible to emulate with conventional modems.

## Equipment & Roles

| Equipment | Role |
|---|---|
| **Cisco AS5300** | RAS (Remote Access Server); terminates dial-up calls with MICA digital modems; supports V.90 over E1 trunks |
| **Cisco 2911** | Voice router/gateway; bridges the AS5300 (E1 via VWIC3-1MFT-T1/E1) to analog FXS ports or to SIP/PCMA (VIC3-4FXS + PVDM3) |
| **Hardmodem USB Lenovo (Conexant CX93010)** | Hardware reference modem; eliminates software variables during initial validation |
| **ATA VoIP Grandstream HT503** (1×FXS + 1×FXO) | VoIP↔analog converter; bridges the softmodem (via SIP) through its FXO port to an FXS port on the Cisco 2911 |
| **Linux computer** | Hosts the *slmodem*, data collection tools, and automated tests |

## Topologies

### Topology 1 — Hardmodem Validation

```
Computer → USB → Hardmodem CX93010 → analog line → Cisco 2911 (FXS) → E1 → Cisco AS5300 (MICA)
```

- **Purpose:** validate RAS configuration and confirm V.90 connectivity with a real hardware modem.
- The hardmodem removes software as a variable; if it fails, the problem is in the Cisco equipment configuration.

### Topology 2 — VoIP ATA Integration

```
Computer (slmodem) → SIP/RTP → Grandstream HT503 (FXO) → Cisco 2911 (FXS) → E1 → Cisco AS5300 (MICA)
```

- **Purpose:** test the *slmodem* as a softmodem over SIP, bridged through the ATA VoIP.
- The slmodem connects to the HT503 via SIP; the ATA converts digitized audio samples to analog on the FXS port.
- **Risk:** the ATA's ADC/DAC quality may impact stability of high-speed protocols (V.34, V.90).

### Topology 3 — Direct Digital E1

```
Computer (slmodem) → Sangoma E1 card → E1 → Cisco AS5300 (MICA)
```

- **Purpose:** eliminate the intermediate analog conversion and validate the direct digital path.
- The deployed implementation uses DAHDI, Wanpipe, and libpri with a Sangoma card.

### Topology 4 — Direct SIP-to-E1 Control

```
Computer (slmodem) → SIP/RTP PCMA → Cisco 2911 → E1 → Cisco AS5300 (MICA)
```

- **Purpose:** retain SIP/RTP while removing the HT503 DAC/ADC segment.
- This control distinguishes limitations of the ATA analog conversion from
  behavior inherent to SIP/RTP or the modem implementations.

## Protocols Tested

| Protocol | Modulation | Speed | Notes |
|---|---|---|---|
| **V.21** | FSK | 300 bps | Most basic; validates call signaling |
| **V.22** | PSK/DPSK | 1,200 bps | Introduces equalization |
| **V.32bis** | QAM-128 + Trellis | 14,400 bps | Echo cancellation |
| **V.34** | QAM-1664 + Pre-coding | 33,600 bps | High complexity, trellis coding |
| **V.90** | PCM (↓) + QAM (↑) | 56,000 / 33,600 bps | **Primary target protocol**; requires digital E1 trunk on server |

Tests always run from simplest to most complex. For each protocol: success/failure, negotiated speed, errors/renegotiation, and slmodem debug logs are recorded.

## Technical Context

- The *slmodem* is a BSD-licensed driver from Smart Link Ltd. The DSP component (`dsplibs.o`) is a proprietary x86-32 binary, hosted in Debian's `non-free` repository.
- No open-source project has implemented modems beyond V.22 (spandsp, Bellard's linmodem, Fisher's experimental implementation — all incomplete for V.34/V.90).
- The reverse-engineering effort is incremental: each function is rewritten, linked against the original binary, and unit-tested. The test bench enables **integration tests** (end-to-end connections), which are impossible without real server-side hardware for V.90.
- Relevant voice-band codec patents have expired.

## Validated coverage

| Path | Highest validated connection | End-to-end data check |
|---|---|---|
| Hardmodem through 2911 FXS/E1 | V.90, 46,667/28,800 bit/s client RX/TX | PPP and ICMP; a session also remained connected for more than 24 hours. |
| slmodem through HT503 | V.34, 26.4/24.0 kbit/s RX/TX | RAS command/response probe. |
| slmodem through direct E1 | V.90 | RAS command/response probe. |
| slmodem through SIP-to-E1 | V.90, up to 56/28.8 kbit/s RX/TX | RAS command/response probe. |

Exact repetitions and rate ranges belong to the matrices under
[`tests/`](../tests/README.md), while the current equipment settings belong to
[`configs/`](../configs/README.md).
