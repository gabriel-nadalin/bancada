# DAHDI/Wanpipe E1 bench

Host: `bancada-dialup`

The Sangoma PCI card connects the Linux host directly to AS5300 controller
E1 1. The link carries EuroISDN PRI with CCS/HDB3/CRC4 framing, D-channel in
timeslot 16, and no echo cancellation on the B-channels.

## Canonical configuration

The active files are versioned once under [`configs/linux/`](../configs/linux/README.md):

| Live path | Repository copy |
|---|---|
| `/etc/wanpipe/wanpipe1.conf` | [`configs/linux/wanpipe1.conf`](../configs/linux/wanpipe1.conf) |
| `/etc/wanpipe/wanrouter.rc` | [`configs/linux/wanrouter.rc`](../configs/linux/wanrouter.rc) |
| `/etc/dahdi/system.conf` | [`configs/linux/system.conf`](../configs/linux/system.conf) |

The matching AS5300 configuration is in
[`configs/as5300/running-config.txt`](../configs/as5300/running-config.txt).
Do not duplicate these stanzas in experiment notes; link to the canonical
files so that configuration audits remain mechanical.

## Hardware and packages

PCI enumeration reports `1923:0040`, and Wanpipe identifies the board as an
`AFT-A101-SH PCI T1/E1` card at bus 4, slot 2. The audited software versions
were:

```text
dahdi-tools 3.4.0-2
dahdi-linux-git 3.4.0.rc1.r21.g276c914-1.2
wanpipe 7.0.38-9.26
libpri 1.6.1-3
```

`wanpipe.service` and `dahdi.service` were enabled and active. They are the
units supplied by the packages; the first runs `wanrouter start`, and the
second runs `dahdi_cfg`.

## Configuration rationale

- `FE_MEDIA = E1`, `FE_LCODE = HDB3`, and `FE_FRAME = CRC4` match the AS5300.
- `TE_SIG_MODE = CCS` and `TDMV_DCHAN = 16` provide the PRI D-channel.
- `ACTIVE_CH = ALL` exposes all 31 E1 timeslots to DAHDI.
- `TDMV_ECHO_OFF = YES`, together with the absence of a DAHDI
  `echocanceller` stanza, keeps modem samples out of voice enhancement paths.
- `TE_CLOCK = NORMAL` makes the Sangoma recover timing from the line. The live
  AS5300 controller also selects line timing; see the clocking note in
  [`configs/README.md`](../configs/README.md).

## Startup and verification

Normal startup is persistent through systemd:

```sh
sudo systemctl enable --now wanpipe.service dahdi.service
```

The same configuration can be reapplied manually with:

```sh
sudo wanrouter start
sudo dahdi_cfg -vv
```

Check the resulting state with:

```sh
sudo dahdi_scan
sudo sed -n '1,80p' /proc/dahdi/1
sudo wanrouter status
```

The audited state was:

```text
active=yes
alarms=OK
name=WPE1/0
devicetype=A101
coding=HDB3
framing=CCS/CRC4
totchans=31
```

`/proc/dahdi/1` showed clear B-channels in 1–15 and 17–31, a
hardware-assisted D-channel in 16, and no attached echo canceller. The Cisco
reported no framing, CRC, code-violation, or slip increments.

## PRI test tool

[`tools/pri_call.c`](../tools/pri_call.c) opens DAHDI channel 16 through
libpri as the EuroISDN CPE/TE side, originates the call, then bridges the
selected B-channel as A-law samples.

For current command-line options and call-state details, see
[`docs/pri_call.md`](pri_call.md). A minimal signaling-only check is:

```sh
sudo tools/pri_call -k
```
