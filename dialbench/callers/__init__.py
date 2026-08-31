"""Caller registry: name -> caller class."""

from ._base import LatencyCaller, ModemCaller
from .baresip import BaresipCaller
from .pjsua import PjsuaCaller
from .rtp_bridge import RtpBridgeCaller
from .slmodem_sip import SlmodemSipCaller
from .slmodem_e1 import SlmodemE1Caller

CALLERS = {
    "baresip": BaresipCaller,
    "pjsua": PjsuaCaller,
    "rtp_bridge": RtpBridgeCaller,
    "slmodem_sip": SlmodemSipCaller,
    "slmodem_e1": SlmodemE1Caller,
}

# A caller is a latency caller if it subclasses LatencyCaller, a modem
# caller if it subclasses ModemCaller. A transport may do both.
LATENCY_CALLERS = {n: c for n, c in CALLERS.items() if issubclass(c, LatencyCaller)}
MODEM_CALLERS = {n: c for n, c in CALLERS.items() if issubclass(c, ModemCaller)}
