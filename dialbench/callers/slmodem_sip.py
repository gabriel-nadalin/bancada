"""slmodem_sip caller: modem call over SIP (rtp_bridge + slmodem_bridge).

This is a modem-call test, not a latency measurement: it establishes the
audio path and lets the slmodem train with the far end, observing/recording
the result. The destination number travels in the SIP peer URI.
"""

import os
import shlex
import sys

from ..paths import TOOLS_DIR
from .. import process
from ._base import (ModemCaller, add_data_probe_args, add_opt,
                    add_slmodem_timing_args, make_data_probe,
                    resolve_slmodem_timing, resolve_v32bis_retrain_snr)


class SlmodemSipCaller(ModemCaller):
    name = "slmodem_sip"
    default_peer = "sip:42@10.42.0.102:5062;transport=udp"

    @staticmethod
    def add_args(sp):
        add_opt(sp, "--slmodem-mode", default="orig", choices=["orig", "ans"],
                help="slmodem mode: orig (default) or ans")
        add_opt(sp, "--record", help="record slmodem RX audio to WAV")
        add_opt(sp, "--tx-record", help="record slmodem TX audio to WAV")
        add_opt(sp, "-M", "--modulation", type=int, default=122,
                help="SREG_DP modulation value "
                     "(21=V.21, 22=V.22, 23=V.23, 122=V.22bis, 132=V.32bis, "
                     "34=V.34, 90=V.90)")
        add_opt(sp, "--debug-level", type=int, default=1,
                help="slmodem debug verbosity (default 1)")
        add_slmodem_timing_args(sp)
        add_opt(sp, "--ptime", type=int, default=10,
                help="frame size in ms (default 10)")
        add_data_probe_args(sp)

    def run_modem(self, opts):
        slm_path = os.path.join(TOOLS_DIR, "slmodem_bridge")
        rtp_path = os.path.join(TOOLS_DIR, "rtp_bridge")

        if not os.path.exists(slm_path):
            print(f"error: {slm_path} not built; run 'make slmodem_bridge'",
                  file=sys.stderr)
            sys.exit(1)

        modem_rate, io_delay, max_rate = resolve_slmodem_timing(opts)
        rtp_cmd = [rtp_path, "-p", opts.peer, "-t", str(opts.ptime)]
        slm_cmd = [slm_path, "-m", opts.slmodem_mode, "-M", str(opts.modulation),
                   "-v", str(opts.debug_level), "-D", str(io_delay),
                   "-S", str(modem_rate)]
        if max_rate is not None:
            slm_cmd += ["-R", str(max_rate)]
        if opts.modulation == 132:
            slm_cmd += ["-N", str(resolve_v32bis_retrain_snr(opts, 9))]
        if getattr(opts, "record", None):
            slm_cmd += ["-r", opts.record]
        if getattr(opts, "tx_record", None):
            slm_cmd += ["-T", opts.tx_record]

        print(f"+ {shlex.join(rtp_cmd)}", file=sys.stderr)
        print(f"+ {shlex.join(slm_cmd)}", file=sys.stderr)

        # slm.stdout (modem TX) -> rtp.stdin; rtp.stdout (RX) -> slm.stdin.
        probe = make_data_probe(opts)
        slm, rtp = process.spawn_pump_pair(
            slm_cmd, rtp_cmd, "slm", "rtp",
            a_stderr_observer=probe.observe_modem_stderr)
        probe.start()
        process.wait_cleanup(slm, rtp, completion=probe)
