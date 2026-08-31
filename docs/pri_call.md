# pri-call-test

Small DAHDI/libpri test utility for originating one PRI call from the Sangoma E1 test bench.

It opens a DAHDI D-channel, runs as PRI CPE/TE, sends one Q.931 `SETUP`, logs libpri events, and hangs up after `CONNECT` or on remote release.

## Build

```sh
cc -O2 -g -Wall -Wextra -o pri-call-test pri-call-test.c -lpri
```

## Usage

```sh
sudo ./pri-call-test [options] <called-number>
```

Useful options:

- `-d <device-or-channel>`: D-channel device or DAHDI channel number, default `16`.
- `-b <channel>`: preferred B-channel, default `1`; use `0` for any channel.
- `-c <caller>`: calling number, default `2000`.
- `-t <seconds>`: hold time after `CONNECT`, default `5`.
- `-a`: 3.1 kHz audio with A-law, default.
- `-s`: speech with A-law.
- `-D`: full libpri protocol debug.

Example:

```sh
sudo ./pri-call-test -t 3 1000
```

Keep Q.921 up without placing a call:

```sh
sudo ./pri-call-test -k
```
