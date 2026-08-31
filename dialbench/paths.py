"""Paths and DSP constants for dialbench."""

import os

# --- DSP constants ---------------------------------------------------------
SRATE = 8000
WINDOW_MS = 10                      # Goertzel window
AMP = 0.5 * 32767

# --- paths -----------------------------------------------------------------
PKG_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_DIR = os.path.dirname(PKG_DIR)     # the bench repo root

# The C transport tools (rtp_bridge, slmodem_bridge, baresip_play).
TOOLS_DIR = os.path.join(REPO_DIR, "tools")

# Topology-3 E1 tool (pri_call), a first-class tool in tools/.
PRI_CALL = os.path.join(TOOLS_DIR, "pri_call")

DATA_DIR = os.path.join(REPO_DIR, "audios")
TX_WAV = os.path.join(DATA_DIR, "bench_tx.wav")
