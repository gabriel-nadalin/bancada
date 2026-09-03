"""dialbench - drive the dial-up bench: measure audio-path latency and run
modem calls across transports (SIP via baresip/pjsua/rtp_bridge, E1 via
slmodem_e1).

Layers:
  gen      create a TX burst wav                    (signal.py)
  analyze  compare TX and RX wavs, report delays    (analysis.py)
  latency  full benchmark: gen -> call -> analyze   (callers/*)
  modem    establish a modem call, observe training (callers/*)

DSP constants and repo paths live in paths.py (source of truth).  See
README.md for the package manual.
"""
