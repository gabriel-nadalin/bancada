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
