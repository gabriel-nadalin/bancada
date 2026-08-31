"""baresip_play caller: latency measurement over a SIP call via baresip."""

import os
import shlex
import subprocess
import sys

from ..paths import TOOLS_DIR, DATA_DIR
from ._base import LatencyCaller


class BaresipCaller(LatencyCaller):
    name = "baresip"
    default_peer = "sip:11@10.42.0.102:5062;transport=udp"

    def run_latency(self, opts):
        rx = opts.rx_wav or os.path.join(DATA_DIR, "baresip_rx.wav")
        cmd = [os.path.join(TOOLS_DIR, "baresip_play"),
               "-p", opts.peer, "-d", opts.tx_wav, "-D", rx]
        print(f"+ {shlex.join(cmd)}", file=sys.stderr)
        subprocess.check_call(cmd)
        return rx
