# Cisco AS5300

[`running-config.txt`](running-config.txt) matches the configuration captured
from the live serial console and contains no credentials.

## Bench mapping

- `controller E1 0` connects to the Cisco 2911: ten B-channels plus the
  D-channel in timeslot 16; `Serial0:15` delivers calls to MICA modems.
- `controller E1 1` connects to the computer's Sangoma card: 30 B-channels
  plus the D-channel; `Serial1:15` emulates the PRI network side.
- All 240 MICA channels run portware `mica-modem-pw.2.9.5.0.bin`.
- `Group-Async1` aggregates the channels, enables interactive PPP, and assigns
  client addresses from `192.168.7.100` through `192.168.7.150`.
- The RAS endpoint in the PPP session is `192.168.7.25`.

## Restoration and verification

Paste the configuration in global configuration mode through the console and
run `copy running-config startup-config`. Then check:

```text
show controller E1 0
show controller E1 1
show isdn status
show modem version
show modem
show running-config interface Group-Async1
show archive config differences nvram:startup-config system:running-config
```

During the audit, E1 0 had an established Q.921 layer to the 2911 and both
physical links had no alarms. The chassis periodically emits
`%RPS-3-MULTFAIL`; that message concerns the redundant power subsystem, not a
configuration mismatch.
