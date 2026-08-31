"""pjsua caller: latency measurement over a SIP call via pjsua."""

import os
import shlex
import subprocess
import threading
import time
import sys

from ..paths import DATA_DIR
from ._base import LatencyCaller


class PjsuaCaller(LatencyCaller):
    name = "pjsua"
    default_peer = "sip:11@10.42.0.102:5062;transport=udp"

    @staticmethod
    def add_args(sp):
        sp.add_argument("--auto-answer", action="store_true",
                        help="auto-answer incoming calls")

    def run_latency(self, opts):
        rx = opts.rx_wav or os.path.join(DATA_DIR, "pjsua_rx.wav")
        cmd = [
            "pjsua",
            "--id", "sip:12@10.42.0.1",
            "--null-audio",
            "--play-file", opts.tx_wav,
            "--auto-play",
            "--auto-play-hangup",
            "--rec-file", rx,
            "--auto-rec",
            "--no-vad",
            "--jb-max-size=0",
            "--capture-lat=5",
            "--playback-lat=5",
            "--quality=1",
            "--ptime", str(opts.ptime),
            "--dis-codec", "PCMU",
            "--clock-rate", "8000",
            "--ec-tail", "0",
            opts.peer,
        ]
        if opts.auto_answer:
            cmd += ["--auto-answer", "200"]
        print(f"+ {shlex.join(cmd)}", file=sys.stderr)
        proc = subprocess.Popen(cmd, stdin=subprocess.PIPE)

        # pjsua hangs up after play-file finishes (--auto-play-hangup) but
        # doesn't exit -- send 'q' to quit after a safety margin.
        def _quit():
            time.sleep(opts.dur + 5)
            try:
                proc.stdin.write(b"q\n")
                proc.stdin.flush()
            except BrokenPipeError:
                pass

        threading.Thread(target=_quit, daemon=True).start()
        proc.wait()
        return rx
