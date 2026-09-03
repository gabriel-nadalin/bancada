"""dialbench command-line interface."""

import argparse
import os
import sys

from . import analysis, paths, signal
from .callers import CALLERS, LATENCY_CALLERS, MODEM_CALLERS


def _common(sp):
    sp.add_argument("--freq", type=int, default=1000,
                    help="burst sine frequency (default 1000 Hz)")
    sp.add_argument("--burst", type=int, default=100,
                    help="burst length in ms (default 100)")
    sp.add_argument("--period", type=int, default=500,
                    help="burst period in ms (default 500)")
    sp.add_argument("--dur", type=int, default=10,
                    help="duration in seconds (default 10)")
    sp.add_argument("--ptime", type=int, default=10,
                    help="frame size in ms (default 10)")


def _latency_common(sp):
    sp.add_argument("--tx-wav", default=paths.TX_WAV,
                    help="TX wav to play (default: %(default)s)")
    sp.add_argument("--rx-wav", default=None,
                    help="RX wav to record (default: caller-specific)")
    sp.add_argument("--peer", default=None,
                    help="peer SIP URI to dial (default: caller-specific)")


def _add_caller_args(sp, caller_classes):
    for cls in caller_classes.values():
        add = getattr(cls, "add_args", None)
        if add:
            add(sp)


def cmd_gen(args):
    n_bursts = signal.generate_tx_wav(args.tx_wav, args.dur, args.freq,
                                      args.burst, args.period)
    print(f"{args.tx_wav}: {args.dur} s, {n_bursts} bursts "
          f"({args.freq} Hz, {args.burst} ms every {args.period} ms), "
          f"first at 0 ms", file=sys.stderr)


def cmd_analyze(args):
    ret = analysis.analyze_pair(args.tx_wav, args.rx_wav, args.freq,
                                args.period, args.burst, csv=args.csv)
    if ret:
        sys.exit(ret)


def _ensure_tx_wav(args):
    if not os.path.exists(args.tx_wav):
        print(f"generating {args.tx_wav} ...", file=sys.stderr)
        signal.generate_tx_wav(args.tx_wav, args.dur, args.freq,
                               args.burst, args.period)


def cmd_latency(args):
    cls = CALLERS[args.caller]
    args.peer = args.peer or cls.default_peer
    caller = cls()
    _ensure_tx_wav(args)
    rx = caller.run_latency(args)
    ret = analysis.analyze_pair(args.tx_wav, rx, args.freq, args.period,
                                args.burst)
    if ret:
        sys.exit(ret)


def cmd_modem(args):
    cls = CALLERS[args.caller]
    args.peer = args.peer or cls.default_peer
    caller = cls()
    caller.run_modem(args)


def main():
    p = argparse.ArgumentParser(prog="dialbench", description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="mode", required=True)

    g = sub.add_parser("gen", help="create TX burst wav")
    g.add_argument("tx_wav")
    _common(g)
    g.set_defaults(fn=cmd_gen)

    an = sub.add_parser("analyze", help="pair TX/RX wavs, report delays")
    an.add_argument("tx_wav")
    an.add_argument("rx_wav")
    an.add_argument("--csv")
    _common(an)
    an.set_defaults(fn=cmd_analyze)

    lat = sub.add_parser("latency", help="full latency benchmark via a caller")
    lat.add_argument("caller", choices=sorted(LATENCY_CALLERS))
    _latency_common(lat)
    _common(lat)
    _add_caller_args(lat, LATENCY_CALLERS)
    lat.set_defaults(fn=cmd_latency)

    mod = sub.add_parser("modem", help="establish a modem call via a caller")
    mod.add_argument("caller", choices=sorted(MODEM_CALLERS))
    mod.add_argument("--peer", default=None,
                     help="peer SIP URI to dial (default: caller-specific)")
    _add_caller_args(mod, MODEM_CALLERS)
    mod.set_defaults(fn=cmd_modem)

    a = p.parse_args()
    a.fn(a)


if __name__ == "__main__":
    main()
