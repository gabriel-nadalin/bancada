# Summary — Dial-Up Test Bench for slmodem Reverse Engineering

## Project Goal

Build a real dial-up test bench using a **Cisco AS5300** (RAS with MICA digital modems and E1 interfaces) to validate the reconstruction of the `dsplibs.o` binary from the *slmodem* — a Linux softmodem driver whose DSP module was never released as source code. The bench enables real connections, especially **V.90** (56 kbps downstream), which requires a digital E1 trunk on the server side — impossible to emulate with conventional modems.

## Equipment & Roles

| Equipment | Role |
|---|---|
| **Cisco AS5300** | RAS (Remote Access Server); terminates dial-up calls with MICA digital modems; supports V.90 over E1 trunks |
| **Cisco 2900** | Voice router/gateway; bridges the AS5300 (E1 via VWIC3-1MFT-T1/E1) to analog FXS ports (VIC3-4FXS + PVDM3) |
| **Hardmodem USB Lenovo (Conexant CX93010)** | Hardware reference modem; eliminates software variables during initial validation |
| **ATA VoIP Grandstream HT503** (1×FXS + 1×FXO) | VoIP↔analog converter; bridges the softmodem (via SIP) to the FXS port on the Cisco 2900 |
| **Linux computer** | Hosts the *slmodem*, data collection tools, and automated tests |

## Topologies

### Topology 1 — Hardmodem Validation

```
Computer → USB → Hardmodem CX93010 → analog line → Cisco 2900 (FXS) → E1 → Cisco AS5300 (MICA)
```

- **Purpose:** validate RAS configuration and confirm V.90 connectivity with a real hardware modem.
- The hardmodem removes software as a variable; if it fails, the problem is in the Cisco equipment configuration.

### Topology 2 — VoIP ATA Integration

```
Computer (slmodem) → SIP/RTP → Grandstream HT503 → FXS → Cisco 2900 → E1 → Cisco AS5300 (MICA)
```

- **Purpose:** test the *slmodem* as a softmodem over SIP, bridged through the ATA VoIP.
- The slmodem connects to the HT503 via SIP; the ATA converts digitized audio samples to analog on the FXS port.
- **Risk:** the ATA's ADC/DAC quality may impact stability of high-speed protocols (V.34, V.90).

### Topology 3 — Direct Digital Connection (advanced phase)

```
Computer (slmodem) → E1 PCIe card (Sangoma A102 or FPGA) → E1 → Cisco AS5300 (MICA)
```

- **Purpose:** eliminate the intermediate analog conversion; simulate line conditions (attenuation, distortion, echo) entirely in software.
- Full control over the test environment. Achievable via a Sangoma A102 card or a student-developed FPGA-based E1 interface.

> **Note:** The AS5300 has up to 8 E1 ports and the 2900 has 4 FXS ports — all three topologies can operate simultaneously.

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

## Timeline (12 months)

| Phase | Activity |
|---|---|
| **1 — Foundation** | Study Cisco AS5300, 2900, and E1/E&M signaling documentation |
| **2 — Topology 1** | Configure Cisco equipment + hardmodem tests (V.34, V.90) |
| **3 — Topology 2** | ATA VoIP integration + SIP; V.21→V.90 tests over SIP; log collection |
| **4 — Topology 3** | Direct E1 connection (Sangoma A102 or FPGA), if viable |
| **Documentation** | Partial report (after Phase 2) + final report + config scripts versioned in Git |
