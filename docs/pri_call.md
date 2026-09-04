# `pri_call`

`tools/pri_call` is the DAHDI/libpri endpoint used by the direct E1 topology.
It can keep Q.921 active, originate a single PRI call, or bridge the selected
B-channel between DAHDI A-law and 8 kHz signed 16-bit PCM on standard I/O.

## Build

```sh
make -C tools pri_call
```

The build requires DAHDI headers and `libpri`; it also compiles the G.711
converter from the pinned `re` submodule.

## Usage

```text
sudo tools/pri_call [options] <called-number>
```

Useful options:

- `-d <device>`: D-channel device or channel number; default `16`.
- `-c <caller>`: calling number; default `11`.
- `-b <channel>`: preferred B-channel; `0` allows any, default `1`.
- `-t <seconds>`: hold time after `CONNECT`; default `5`.
- `-p <seconds>`: delay between D-channel establishment and `SETUP`;
  default `1`.
- `-n cpe|network`: PRI node type; default `cpe`.
- `-w euroisdn|ni2`: switch type; default `euroisdn`.
- `-k`: keep Q.921 active without placing a call.
- `-a`: 3.1 kHz audio with A-law bearer capability; default.
- `-s`: speech with A-law bearer capability.
- `-A`: bridge the connected B-channel as 8 kHz PCM until interrupted.
- `-D`: enable full libpri protocol debugging.

Use `-` as the called number to send an empty Called Party Number information
element.

## Examples

Keep the D-channel active without originating a call:

```sh
sudo tools/pri_call -k
```

Call through B-channel 2 and hold for three seconds:

```sh
sudo tools/pri_call -b 2 -t 3 42
```

Bridge modem audio for `dialbench`:

```sh
sudo tools/pri_call -A -b 2 42
```

In audio mode, `pri_call` opens the B-channel as soon as Q.931 reports the
selected channel in `PROCEEDING`, so the client can receive early modem
training audio before `CONNECT`.
