# Bench configurations

This directory is the canonical source for the configurations that determine
the experiments' telephony path. The snapshots were checked against the live
equipment on September 4, 2026. They contain no passwords, serial numbers, or
SIP credentials.

| Component | Canonical configuration | Purpose |
|---|---|---|
| Linux computer | [`linux/`](linux/README.md) | Shared network, Wanpipe, and DAHDI for direct E1. |
| Cisco 2911 | [`cisco2911/`](cisco2911/README.md) | FXS, SIP, PCMA conversion, and PRI gateway to the RAS. |
| Cisco AS5300 | [`as5300/`](as5300/README.md) | PRI, MICA modems, and PPP termination. |
| Grandstream HT503 | [`ht503/`](ht503/README.md) | SIP/PCMA to analog FXO bridge. |

## Restoration order

1. Restore the Linux computer's shared network. It provides `10.42.0.1/24`,
   DHCP, and DNS on `enp1s0`.
2. Restore the AS5300 and Cisco 2911 through their console ports.
3. Apply the HT503 FXO profile through its web interface, then reboot it.
4. Start Wanpipe and DAHDI only when using the direct E1 link between the
   computer and the AS5300.
5. Run each device's README checks before running modem experiments.

The addresses `10.42.0.22` (Cisco 2911) and `10.42.0.102` (HT503) were DHCP
leases at capture time. Test commands contain them because they were the live
addresses, but the NetworkManager profile does not reserve them. Confirm the
leases after restoration before running `dialbench`.

## E1 clocking

The files reproduce the live selections exactly: the Sangoma uses
`TE_CLOCK = NORMAL`, the 2911 selects E1 as its clock source, and both active
AS5300 E1 controllers use `clock source line`. Do not infer from these files
that the AS5300 is the clock master. The bench showed no alarms or slips during
the audit, but any cabling change calls for a new clock-plan check.
