"""slmodem_e1 caller: modem call over a direct E1 (pri_call + slmodem_bridge).

Topology 3: the slmodem connects straight to the RAS over the Sangoma E1.
pri_call establishes the ISDN PRI call and bridges the B-channel
(A-law <-> 8k linear on stdin/stdout); slmodem_bridge is cross-connected to
it. The modem originates early (-e) so it is already listening when the
audio cuts through. No SIP, no ATA, no analog.
"""

import os
import shlex
import sys

from ..paths import TOOLS_DIR, PRI_CALL
from .. import process
from ._base import ModemCaller, add_opt


class SlmodemE1Caller(ModemCaller):
    name = "slmodem_e1"
    default_peer = None

    @staticmethod
    def add_args(sp):
        add_opt(sp, "--record", help="record slmodem RX audio to WAV")
        add_opt(sp, "-M", "--modulation", type=int, default=90,
                help="SREG_DP modulation value "
                     "(21=V.21, 122=V.22bis, 132=V.32bis, 34=V.34, 90=V.90)")
        add_opt(sp, "-b", "--bchannel", type=int, default=2,
                help="preferred B-channel (default 2)")
        add_opt(sp, "--called", default="-",
                help="called number, or '-' for empty (default '-')")

    def run_modem(self, opts):
        slm_path = os.path.join(TOOLS_DIR, "slmodem_bridge")
        if not os.path.exists(slm_path):
            print(f"error: {slm_path} not built; run 'make -C tools'",
                  file=sys.stderr)
            sys.exit(1)
        if not os.path.exists(PRI_CALL):
            print(f"error: {PRI_CALL} not built; run 'make -C tools'",
                  file=sys.stderr)
            sys.exit(1)

        slm_cmd = [slm_path, "-m", "orig", "-M", str(opts.modulation), "-e"]
        if getattr(opts, "record", None):
            slm_cmd += ["-r", opts.record]

        # pri_call needs root for /dev/dahdi; sudo prompts on the tty.
        pct_cmd = ["sudo", PRI_CALL, "-A", "-b", str(opts.bchannel),
                   opts.called]

        print(f"+ {shlex.join(slm_cmd)}", file=sys.stderr)
        print(f"+ {shlex.join(pct_cmd)}", file=sys.stderr)

        # slm.stdout (modem TX) -> pct.stdin; pct.stdout (RX) -> slm.stdin.
        slm, pct = process.spawn_pump_pair(slm_cmd, pct_cmd, "slm", "pct")
        process.wait_cleanup(slm, pct)
