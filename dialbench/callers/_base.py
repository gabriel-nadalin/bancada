"""Caller interfaces.

A caller = one way to establish a call path. A transport may support the
latency measurement (play TX wav, capture RX), the modem-call test (observe
training), or both -- implement the corresponding method(s).

Callers also declare a default SIP peer and may register CLI args via a
classmethod ``add_args(sp)`` (all optional flags, added to the shared parser).
"""


def add_opt(sp, *args, **kwargs):
    """Add an argparse option only if it isn't already defined.

    Callers share one parser per command, so two callers may declare the
    same option name (e.g. --record on both modem callers); skip the later
    declaration instead of letting argparse raise a conflict.
    """
    for a in args:
        if a in sp._option_string_actions:
            return
    sp.add_argument(*args, **kwargs)


def add_data_probe_args(sp):
    """Add the mandatory end-to-end RAS data-probe options."""
    add_opt(sp, "--probe-command", default="show clock",
            help="RAS command sent through the slmodem PTY "
                 "(default: show clock)")
    add_opt(sp, "--probe-expect", default="UTC",
            help="text expected in each RAS response (default: UTC)")
    add_opt(sp, "--probe-prompt", default="Router>",
            help="RAS prompt expected after each response (default: Router>)")
    add_opt(sp, "--probe-count", type=int, default=3,
            help="number of successful data probes required (default: 3)")
    add_opt(sp, "--probe-max-attempts", type=int, default=None,
            help="maximum command transmissions allowed "
                 "(default: required responses plus 2)")
    add_opt(sp, "--probe-interval", type=float, default=5.0,
            help="seconds between data probes (default: 5)")
    add_opt(sp, "--probe-settle", type=float, default=3.0,
            help="seconds to wait after CONNECT before sending data "
                 "(default: 3)")
    add_opt(sp, "--probe-connect-timeout", type=float, default=120.0,
            help="seconds allowed for modem connection (default: 120)")
    add_opt(sp, "--probe-response-timeout", type=float, default=15.0,
            help="seconds allowed per RAS response (default: 15)")


def make_data_probe(opts):
    """Create the mandatory PTY probe from parsed caller options."""
    from ..process import PtyDataProbe

    if opts.debug_level < 1:
        raise ValueError("--debug-level must be at least 1 so the mandatory "
                         "probe can observe CONNECT")
    if opts.probe_count < 1:
        raise ValueError("--probe-count must be at least 1")
    max_attempts = opts.probe_max_attempts
    if max_attempts is None:
        max_attempts = opts.probe_count + 2
    if max_attempts < opts.probe_count:
        raise ValueError("--probe-max-attempts cannot be less than "
                         "--probe-count")
    if opts.probe_interval < 0:
        raise ValueError("--probe-interval cannot be negative")
    if opts.probe_settle < 0:
        raise ValueError("--probe-settle cannot be negative")
    if opts.probe_connect_timeout <= 0:
        raise ValueError("--probe-connect-timeout must be positive")
    if opts.probe_response_timeout <= 0:
        raise ValueError("--probe-response-timeout must be positive")
    required_rate = None
    if opts.modulation == 132:
        required_rate = min(opts.max_rate or 14400, 14400)
    elif opts.modulation == 32:
        required_rate = min(opts.max_rate or 9600, 9600)

    return PtyDataProbe(
        command=opts.probe_command,
        expected=opts.probe_expect,
        prompt=opts.probe_prompt,
        count=opts.probe_count,
        max_attempts=max_attempts,
        interval=opts.probe_interval,
        settle=opts.probe_settle,
        connect_timeout=opts.probe_connect_timeout,
        response_timeout=opts.probe_response_timeout,
        required_rate=required_rate)


def add_slmodem_timing_args(sp):
    """Add native-rate and reported-I/O-delay overrides.

    V.32/V.32bis run internally at 8 kHz, while V.34/V.90 use the 9.6 kHz
    VPCM pump.  Leaving both options unset selects the matching profile.
    """
    add_opt(sp, "--modem-rate", type=int, choices=[8000, 9600],
            default=None,
            help="native slmodem data-pump rate "
                 "(default: 8000 for V.32/V.32bis, 9600 otherwise)")
    add_opt(sp, "--io-delay", type=int, default=None,
            help="reported local audio I/O delay, in native-rate samples "
                 "(default: 0 at 8000 Hz, 240 at 9600 Hz)")
    add_opt(sp, "--max-rate", type=int, default=None,
            help="maximum data rate in bit/s "
                 "(default: DSP maximum)")
    add_opt(sp, "--v32bis-retrain-snr", type=int, default=None,
            help="local V.32bis retrain SNR threshold in dB "
                 "(default: 13 on E1, 9 on SIP)")


def resolve_slmodem_timing(opts):
    """Return native rate, local I/O delay, and an optional data-rate cap."""
    is_v32 = opts.modulation in (32, 132)
    modem_rate = opts.modem_rate
    if modem_rate is None:
        modem_rate = 8000 if is_v32 else 9600

    io_delay = opts.io_delay
    if io_delay is None:
        io_delay = 0 if modem_rate == 8000 else 240
    if io_delay < 0:
        raise ValueError("--io-delay cannot be negative")

    max_rate = opts.max_rate
    if max_rate is not None and not 300 <= max_rate <= 56000:
        raise ValueError("--max-rate must be between 300 and 56000 bit/s")
    return modem_rate, io_delay, max_rate


def resolve_v32bis_retrain_snr(opts, default):
    """Return the caller-specific V.32bis local retrain threshold."""
    threshold = opts.v32bis_retrain_snr
    if threshold is None:
        threshold = default
    if not 0 <= threshold <= 40:
        raise ValueError("--v32bis-retrain-snr must be between 0 and 40 dB")
    return threshold


class LatencyCaller:
    """Establishes a call, plays a TX wav through the transport, captures RX.

    ``run_latency(opts)`` returns the path of the captured RX WAV.
    """
    name = None
    default_peer = None

    def run_latency(self, opts):
        raise NotImplementedError


class ModemCaller:
    """Establishes a modem call and observes training. No latency analysis."""
    name = None
    default_peer = None

    def run_modem(self, opts):
        raise NotImplementedError
