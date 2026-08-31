"""rtp_bridge caller: latency measurement over a SIP call via rtp_bridge."""

import os
import shlex
import subprocess
import sys

from ..paths import TOOLS_DIR, DATA_DIR
from ._base import LatencyCaller


class RtpBridgeCaller(LatencyCaller):
    name = "rtp_bridge"
    default_peer = "sip:11@10.42.0.102:5062;transport=udp"

    def run_latency(self, opts):
        rx = opts.rx_wav or os.path.join(DATA_DIR, "bridge_rx.wav")
        cmd = [os.path.join(TOOLS_DIR, "rtp_bridge"),
               "-p", opts.peer, "-i", opts.tx_wav, "-o", rx,
               "-t", str(opts.ptime)]
        print(f"+ {shlex.join(cmd)}", file=sys.stderr)
        subprocess.check_call(cmd)
        return rx
