# Cisco 2911

[`running-config.txt`](running-config.txt) is the configuration captured from
the live USB console. Only the last-change timestamp and the serial number from
`license udi` were removed; the running configuration contained no passwords.

## Material settings

- `GigabitEthernet0/0` uses DHCP and held `10.42.0.22` during the audit.
- The VoIP trusted list allows only the bench host, `10.42.0.1`.
- SIP signaling and RTP media are bound to `GigabitEthernet0/0`.
- VoIP dial-peer 142 accepts called number 42 using PCMA and hands it to POTS
  dial-peer 42 on PRI `0/0/0:15`.
- The E1 uses EuroISDN (`primary-net5`) and only timeslots `1-10,16`, matching
  the available PVDM capacity.
- Echo cancellation, nonlinear processing, and VAD are disabled on modem
  paths; all four FXS ports use A-law.
- Dial-peers 10 through 13 map to FXS ports `0/1/0` through `0/1/3`.

## Restoration and verification

On the console, enter privileged mode, apply the file in global configuration
mode, and save it with `copy running-config startup-config`. Then run:

```text
show ip interface brief
show controller E1 0/0/0
show isdn status
show dial-peer voice summary
show running-config | section voice service voip
show archive config differences nvram:startup-config system:running-config
```

The audited state had `GigabitEthernet0/0` and `Serial0/0/0:15` in `up/up`,
Q.921 layer 2 in `MULTIPLE_FRAME_ESTABLISHED`, no E1 alarms, and no difference
between running and startup configurations.
