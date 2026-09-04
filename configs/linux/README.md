# Linux computer

The four files are credential-free copies of the active configuration:

| Repository file | Destination |
|---|---|
| [`wanpipe1.conf`](wanpipe1.conf) | `/etc/wanpipe/wanpipe1.conf` |
| [`wanrouter.rc`](wanrouter.rc) | `/etc/wanpipe/wanrouter.rc` |
| [`system.conf`](system.conf) | `/etc/dahdi/system.conf` |
| [`sip-ata.nmconnection`](sip-ata.nmconnection) | `/etc/NetworkManager/system-connections/SIP ATA.nmconnection` |

The volatile NetworkManager `timestamp` field was omitted. Every other field,
including the UUID, matches the active profile. IPv4 `shared` mode assigns
`10.42.0.1/24` to `enp1s0` and starts NetworkManager's internal DHCP/DNS
service.

## Audited versions

```text
dahdi-tools 3.4.0-2
dahdi-linux-git 3.4.0.rc1.r21.g276c914-1.2
wanpipe 7.0.38-9.26
libpri 1.6.1-3
pjproject 2.17-1.1
```

## Restoration

Copy the files as `root`, restrict the NetworkManager profile to mode `0600`,
and reload the configurations:

```sh
sudo install -m 0644 configs/linux/wanpipe1.conf /etc/wanpipe/wanpipe1.conf
sudo install -m 0644 configs/linux/wanrouter.rc /etc/wanpipe/wanrouter.rc
sudo install -m 0644 configs/linux/system.conf /etc/dahdi/system.conf
sudo install -m 0600 configs/linux/sip-ata.nmconnection \
  '/etc/NetworkManager/system-connections/SIP ATA.nmconnection'
sudo nmcli connection reload
sudo nmcli connection up 'SIP ATA'
sudo systemctl enable --now wanpipe.service dahdi.service
```

The package-provided units run `wanrouter start` and `dahdi_cfg`; there are no
additional local units. To reapply the telephony configuration manually:

```sh
sudo wanrouter start
sudo dahdi_cfg -vv
```

## Verification

```sh
ip -4 address show dev enp1s0
sudo dahdi_scan
sudo sed -n '1,80p' /proc/dahdi/1
sudo wanrouter status
```

Expect `10.42.0.1/24` on `enp1s0`, an active CCS/HDB3/CRC4 E1 span, a
hardware-assisted D-channel in timeslot 16, B-channels in every other
timeslot, and no attached echo canceller.
