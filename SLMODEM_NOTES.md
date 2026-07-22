# slmodem + rtp_bridge integration notes

## Goal

Use the Smart Link softmodem (`slmodem`) as the modem DSP and `rtp_bridge` as
the RTP transport, so a software modem on the host can talk to a hardware
modem (Conexant) through the Grandstream ATA / Cisco 2911 network.

```
[host: slmodem + rtp_bridge] --RTP--> [ATA] --E1--> [Cisco] --FXS--> [Conexant]
```

## What changed

- `slmodem-2.9.11-20110321/modem/modem_main.c`  
  Added `#include <sys/sysmacros.h>` so the 32-bit build works on modern
  glibc (`minor()` is no longer in `<sys/types.h>`).

- `slmodem_bridge.c` (new)  
  Wraps one `slmodem` instance and exposes:
  - **stdin**:  8 kHz mono s16le PCM from the network (`rtp_bridge`)
  - **stdout**: 8 kHz mono s16le PCM to the network (`rtp_bridge`)
  - **PTY**:    AT commands and user data

  Internally it resamples 8 kHz <-> 9.6 kHz because `slmodem`'s data-pump
  runs at 9600 Hz.

- `Makefile`  
  Added `slmodem_bridge` target. It compiles with `-m32` and links the
  slmodem object files plus 32-bit `libsamplerate`.

- `burstbench.py`  
  Added `run slmodem` subcommand that spawns and cross-connects
  `rtp_bridge` and `slmodem_bridge`.

## Build

```bash
make slmodem_bridge
```

This produces the 32-bit `slmodem_bridge` executable.

## Manual run

```bash
mkfifo /tmp/slm_to_rtp /tmp/rtp_to_slm

./rtp_bridge -p sip:11@10.42.0.102:5062 -t 10 </tmp/rtp_to_slm >/tmp/slm_to_rtp &
./slmodem_bridge -m orig </tmp/slm_to_slm >/tmp/slm_to_rtp &
```

`slmodem_bridge` prints its PTY, e.g.:

```
PTY: /dev/pts/4
```

Write data / AT commands there and read received data from the same PTY.

## Orchestrated run

```bash
python3 burstbench.py run slmodem --peer sip:11@10.42.0.102:5062
```

This starts both processes, cross-connects their PCM streams, and echoes
stderr including the PTY name.

## Modes

- `orig` (default): sends `ATD` and acts as the calling modem.
- `ans`: sends `ATA` and acts as the answering modem.

For the current setup the host is the caller, so use `orig`.

## Known limitations

- Local loopback of two `slmodem_bridge` instances goes off-hook but did not
  complete training on this machine. This machine has no hardware attached,
  so the real test must be done on the hardware box.
- Resampling 9600 Hz <-> 8000 Hz is done per 10 ms frame using
  libsamplerate (`SRC_SINC_FASTEST`). If training fails on hardware, try
  a higher-quality resampler or running the network path at 9600 Hz.
- `slmodem_bridge` currently terminates without hanging up the modem call.
  On the hardware box you may want to send `ATH` to the PTY before killing
  the process.

## Files to copy to the hardware machine

- `slmodem_bridge.c`
- `Makefile`
- `slmodem-2.9.11-20110321/modem/modem_main.c` (build fix)
- `burstbench.py` (if you want the orchestrated command)

Then run `make slmodem_bridge` on the hardware box.

## Hardware box prerequisites

- 32-bit toolchain: `gcc-multilib` or `gcc -m32` plus 32-bit libc.
- 32-bit `libsamplerate`: on Debian/Ubuntu, `libsamplerate0:i386`.
- The slmodem source tree (`slmodem-2.9.11-20110321/`) must be present,
  including the `modem_main.c` build fix.
