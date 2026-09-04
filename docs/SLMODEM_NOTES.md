# slmodem integration with E1 and SIP

## Architecture

`slmodem_bridge` connects the slmodem data interface to a PTY and exchanges
8 kHz, mono, linear PCM through standard input and output. The PCM stream can
go directly to an E1 B-channel through `pri_call`, or to RTP/PCMA through
`rtp_bridge`.

The proprietary `dsplibs.o` object contains data pumps with two native sample
rates:

| Data pump | Native rate | Bench profile |
|---|---:|---:|
| V.32 | 8,000 Hz | 8,000 Hz, no resampling, `IODELAY=0` |
| V.32bis | 8,000 Hz | 8,000 Hz, `IODELAY=0`, 14,400 bit/s maximum; local retrain SNR 13 dB on E1, 9 dB on SIP |
| VPCM (V.34/V.90) | 9,600 Hz | 9,600 Hz, `IODELAY=240` |
| Other data pumps | 8,000 Hz internally | legacy 9,600 Hz profile |

The `--modem-rate`, `--io-delay`, `--max-rate`, and
`--v32bis-retrain-snr` options allow controlled experiments. Without explicit
overrides, `dialbench` selects the profiles above. Other data pumps retain
their DSP maximum. V.90 remains available only on the E1 path.

## Meaning of `IODELAY`

`MDMCTL_IODELAY` is not network delay or link RTT. It represents the local
capture/playback queue skew, measured in samples at the rate exposed to the
modem.

The original `slmodemd` has more than one device backend. Its default is the
`modemap` backend over `/dev/slamr0`, used with dedicated kernel drivers,
including PCI modem hardware. It primes playback with 192 samples and adds
that userspace prefill to the delay returned by the device driver's
`MDMCTL_IODELAY` ioctl. The optional ALSA backend instead queues playback
silence before capture starts and reports that queue plus 40 samples of
internal delay. In both cases the value describes a concrete local audio
queue, not arbitrary transport latency. A negative update allows the playback
queue to drain while capture samples are discarded; a positive update inserts
playback silence.

The reconstructed `dsplibs.o` logic reveals two different consumers:

- V.32 adds 48 to `IODELAY`, clamps physical delay to 216, and converts it to
  the data pump's round-trip parameter. The queue-free bridge must report
  zero.
- VPCM requires 9,600 Hz and adds 4 to `IODELAY`. If the resulting value is
  greater than 244, it requests removal of the excess; smaller values remain
  smaller. Reporting 240 places its initial delay exactly at the nominal
  244-sample boundary without an update. This known-good value must therefore
  be preserved for V.34 and V.90.

Reporting 240 globally caused V.32 to request a 72-sample reduction while
also passing the signal through two consecutive conversions: 8→9.6 kHz in
the bridge, followed by 9.6→8 kHz in the internal wrapper. Running the native
8 kHz path removes both conversions and the artificial delay update. This is
the correct queue model for the bridge, but delay sweeps and the native path
alone did not eliminate the intermittent V.32bis fallback.

## V.32bis diagnosis and correction

The fallback was a local `dsplibs.o` policy decision, not a throughput cap,
RAS request, log-overhead problem, or MICA speed shift. The reconstructed
`V32FP_status()` function smooths its decision error into an internal SNR
estimate and tests it every 5 ms. After the initial 200-call settling period,
more than eight consecutive samples below the rate-specific value in
`SnrToRetrainTable` cause the local modem to request a retrain. The original
six-entry table in the binary is `9, 13, 13, 11, 20, 24` dB; the last two
entries apply at 12 and 14.4 kbit/s.

On failing E1 calls, detailed traces showed a short estimated-SNR sequence
falling through 14, 13, and 12 dB, followed by the explicit message
`8 coseq SNR drops detected local retrain is initiated`. The local modem then
requested retrain and stepped from 14.4/14.4 to 12/12 kbit/s. During the same
interval, three RAS command exchanges remained valid, and MICA operational
status reported approximately 42--43 dB SNR with no MICA retrain or speed
shift. The Conexant controls also remain at full rate through the same bench
path. This separates the transient value produced by the old local estimator
from the stationary quality of the E1 channel.

Reducing diagnostic output did not remove the problem. A level-1 control
batch still fell back, as did a direct OS-pipe run with the Python PCM pumps
bypassed. Correct partial-write handling, the original 5 ms process boundary,
TX-level experiments, and the tested `IODELAY` profiles likewise did not
eliminate it. Detailed logging exposed the local trigger but did not cause it.

For V.32bis only, `slmodem_bridge` therefore replaces the 12 and 14.4 kbit/s
local thresholds with values already used elsewhere in the original table.
E1 uses 13 dB, the original 9.6 kbit/s value. SIP uses 9 dB, the original
4.8 kbit/s value, because its detailed traces repeatedly reached 10--12 dB
at approximately -102 ppm without an RTP packet gap or loss of RAS data.
This tolerates the reproducible short estimator dip but does not disable
protection: a sustained estimate below the selected threshold still requests
retrain, and remote-initiated retrain handling is a separate path.

The E1 and SIP callers select their defaults explicitly. The
`--v32bis-retrain-snr` option is available for controlled comparison, not as
a data-rate control. The Makefile uses `objcopy` to export
`SnrToRetrainTable` from a generated bridge-only copy of `dsplibs.o`; the
vendor object itself is not modified. Other data pumps never touch this
table.

## V.32bis full-rate requirement

The direct E1 path is not bandwidth- or clock-limited. `pri_call` opens a
clear-channel DAHDI B-channel, performs one standard linear-PCM/A-law
conversion in each direction, and exchanges 8,000 samples/s with the modem.
After that quantization, the samples remain in a single synchronous digital
clock domain up to the RAS. There is no asynchronous resampler or analog
segment in this path; each generated A-law codeword is transported unchanged.
In an eight-call diagnostic run at 14.4/14.4 kbit/s, every call completed
three real RAS command exchanges, but four calls subsequently retrained to
12/12 kbit/s because of the local policy described above. The E1 channel did
not impose that rate.

The 8 kHz sample rate is also not a Nyquist limitation for this protocol.
V.32/V.32bis uses a 1,800 Hz carrier at 2,400 baud, and the reconstructed
`V32FP` engine itself runs natively at 8 kHz.

The SIP topology includes the HT503 ATA and an analog segment before returning
to G.711. Its converter clock is asynchronous to the bridge clock, and the
V.32 timing loop measured approximately 100 ppm of offset. That can reduce
margin, but it does not justify a rate cap: the Conexant reference modems reach
the full V.32bis rate under the same bench conditions. A SIP or E1 fallback is
therefore treated as a bridge/data-pump integration failure even when data
continues to pass after retraining.

Recommendation V.32bis lists 14.4, 12, 9.6, 7.2, and 4.8 kbit/s as its
signalling rates and states that each modem's transmit and receive rates
shall be equal; asymmetric operation was left for further study. Both E1 and
SIP therefore use the DSP's 14.4 kbit/s maximum by default. `--max-rate`
remains available for controlled diagnostics, not as a production workaround.

## End-to-end data criterion

The `slmodem_e1` and `slmodem_sip` callers always run a data probe through the
PTY three seconds after `CONNECT`, allowing the link to settle before DTE
traffic starts. Each attempt sends `show clock` and succeeds only after
receiving these items in order:

1. the `show clock` command echo;
2. a response line containing `UTC`;
3. the `Router>` prompt.

Three responses are required by default. V.21 carries both directions at
300 bit/s, while an originating V.23 client sends DTE data on the 75 bit/s
backward channel. Their command echo and response can therefore arrive much
later than on V.32bis or V.34. An incomplete early exchange is retained and
retried instead of being discarded. The default permits five transmissions
but still requires three complete responses; it does not turn an unanswered
command into a pass. `--probe-count`,
`--probe-max-attempts`, `--probe-interval`, `--probe-settle`,
`--probe-connect-timeout`, and `--probe-response-timeout` adjust test
duration, but there is no option to skip the probe. A false `CONNECT` without
data traffic therefore fails the test.

For V.32 and V.32bis, the probe additionally observes every negotiated-rate
notification. It fails after completing the real RAS exchanges if either
direction ever leaves the requested rate. A retrain to 12/12 kbit/s can no
longer be reported as a passing 14.4/14.4 kbit/s test merely because the
lower-rate connection still carries data.

The August 31 control runs are a positive baseline, not merely carrier
tests. V.23 returned `show clock` through the slmodem PTY, and the later
V.21/RcFixed run returned both `show clock` and `show users`. The AS5300
reported 392 transmitted characters, 23 received characters, and zero
retrains for that V.21 call. Any build that reaches `CONNECT` but cannot
repeat this exchange is a regression or an invalid test run.

Examples:

```bash
python -m dialbench modem slmodem_e1 -M 22 -b 2
python -m dialbench modem slmodem_sip -M 122
python -m dialbench modem slmodem_e1 -M 132 -b 2
python -m dialbench modem slmodem_sip -M 132
python -m dialbench modem slmodem_e1 -M 90 -b 2 --probe-count 5
```

## Bench validation

The final September 1, 2026 regression used the default `RcFixed` build and
required the command echo, `UTC` response, and RAS prompt for every probe:

| Protocol | E1 | SIP | Default profile |
|---|---:|---:|---|
| V.21 | 1 call, 3/3 RAS, 300/300 bit/s | 1 call, 3/3 RAS, 300/300 bit/s | 9,600 Hz, `IODELAY=240` |
| V.22 | 1 call, 3/3 RAS, 1,200/1,200 bit/s | 1 call, 3/3 RAS, 1,200/1,200 bit/s | 9,600 Hz, `IODELAY=240` |
| V.22bis | 1 call, 3/3 RAS, 2,400/2,400 bit/s | 1 call, 3/3 RAS, 2,400/2,400 bit/s | 9,600 Hz, `IODELAY=240` |
| V.23 | 1 call, 3/3 RAS, 1,200/1,200 bit/s | 1 call, 3/3 RAS, 1,200/1,200 bit/s | 9,600 Hz, `IODELAY=240` |
| V.32bis | 4/4 calls, 12/12 RAS, 14.4/14.4 kbit/s | 12/12 calls, 36/36 RAS, 14.4/14.4 kbit/s | 8,000 Hz, `IODELAY=0`; 13 dB E1, 9 dB SIP |
| V.34 | 1 call, 3/3 RAS, 33.6/33.6 kbit/s | 1 call, 3/3 RAS, 24/26.4 kbit/s | 9,600 Hz, `IODELAY=240` |
| V.90 | 1 call, 5/5 RAS, 31.2/56 kbit/s | Not supported | 9,600 Hz, `IODELAY=240` |

V.21 and V.23 needed one retry on both transports because the first response
arrived after the per-attempt window. The retained response then completed,
and two subsequent commands also passed. V.22 and V.22bis were tested
separately rather than treating V.22bis as coverage for its fallback mode;
their negotiated rates matched the Conexant controls.

The final V.32bis tests had no explicit maximum-rate option. Every call stayed
at 14.4/14.4 kbit/s for all three `show caller` exchanges. The E1 set used the
13 dB local threshold and the SIP set used 9 dB. The earlier uncapped E1
diagnostic remains useful negative evidence: all eight calls carried RAS data,
but four later retrained from 14.4/14.4 to 12/12 kbit/s with the stock local
policy. The E1 timing estimate converged to 0 ppm; SIP timing remained between
approximately -102 and -90 ppm in the detailed diagnostic runs.

### V.34 three-path regression, September 3, 2026

V.34 was repeated serially after the SIP port-isolation and RTP diagnostics
changes. Each run required three complete `show clock` exchanges through the
slmodem PTY. All three paths passed:

| Path | Result | Negotiated TX/RX rate | RAS data probe |
|---|---|---:|---:|
| Direct E1 (`slmodem_e1`) | Pass | 33.6/33.6 kbit/s | 3/3 |
| SIP through HT503 | Pass | 24.0/26.4 kbit/s | 3/3 |
| SIP-to-E1 through Cisco 2911 | Pass | 31.2/33.6 kbit/s | 3/3 |

The two SIP calls used ephemeral local SIP and RTP ports. The PCMA RTP
timestamps were contiguous in both cases. This is a regression check of
functional data traffic, not a carrier-only observation.

## Build

The bridge must be compiled as a 32-bit executable because `dsplibs.o` is a
32-bit x86 object:

```bash
make dependencies
make -C tools slmodem_bridge
```

The normal path uses the `RcFixed` converters exported by `dsplibs.o`. The
optional `USE_LIBSAMPLERATE` build exists only for comparison; it is not the
production profile because its sinc path regressed V.90.

Arch Linux requires `lib32-glibc` and `lib32-gcc-libs`. Debian and Ubuntu
require `gcc-multilib` and `libc6-dev-i386`.

## Source references

- [ITU-T V.32bis (1991)](https://www.itu.int/rec/T-REC-V.32bis-199102-I/en),
  clauses 1, 2.1, and 2.2: supported symmetric data rates, 1,800 Hz carrier,
  2,400-symbol/s modulation-rate tolerance, and the V.2 transmit-power
  requirement.
- Original slmodem `modem_main.c`: both the default `modemap` backend for
  dedicated modem drivers and the optional ALSA backend, including startup
  delay, `MDMCTL_IODELAY`, and `update_delay` handling.
- Reconstructed `dsplibs.o` `core/v32.c` and `core/vpcm.c`: the native-rate
  wrappers and delay calculations.
- Reconstructed `dsplibs.o` `v32/V32stc.c`: the SNR estimator, consecutive
  low-SNR counter, local retrain request, and rate fallback path.
