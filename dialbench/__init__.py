"""dialbench - drive the dial-up bench: measure audio-path latency and run
modem calls across transports (SIP via baresip/pjsua/rtp_bridge, later E1).

Layers:
  gen      create a TX burst wav                    (signal.py)
  analyze  compare TX and RX wavs, report delays    (analysis.py)
  latency  full benchmark: gen -> call -> analyze   (callers/*)
  modem    establish a modem call, observe training (callers/*)
"""

import os

# --- DSP constants ---------------------------------------------------------
SRATE = 8000
WINDOW_MS = 10                      # Goertzel window
AMP = 0.5 * 32767

# --- paths -----------------------------------------------------------------
PKG_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_DIR = os.path.dirname(PKG_DIR)     # the bench repo root

# Where the C tools (rtp_bridge, slmodem_bridge, baresip_play) live.
# Step 1: still at the repo root; later moved to tools/ - change here only.
TOOLS_DIR = REPO_DIR

DATA_DIR = os.path.join(REPO_DIR, "audios")
TX_WAV = os.path.join(DATA_DIR, "bench_tx.wav")
