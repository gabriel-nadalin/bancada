# Build the bench's C transport tools (rtp_bridge, slmodem_bridge, baresip_play).
#
# Run from tools/. Vendor trees (re/, baresip/, slmodem-*) are at the repo
# root (REPO_DIR = ..). Binaries build into this directory.

REPO_DIR := ..

CC ?= gcc
OBJCOPY ?= objcopy
CFLAGS = -Wall -Wextra -O2 \
         -I$(REPO_DIR)/re/include \
         -I$(REPO_DIR)/baresip/include

LDFLAGS = -L$(REPO_DIR)/baresip/build \
          -L$(REPO_DIR)/re/build \
          -Wl,-rpath,'$$ORIGIN/../re/build' \
          -lbaresip -lre -lpthread -lm -lssl -lcrypto

SLMODEM_DIR = $(REPO_DIR)/slmodem-2.9.11-20110321/modem
SLMODEM_DSP_OBJ = $(SLMODEM_DIR)/dsplibs.o
SLMODEM_BRIDGE_DSP_OBJ = dsplibs_bridge.o
SLMODEM_OBJS = $(SLMODEM_DIR)/modem.o $(SLMODEM_DIR)/modem_datafile.o \
               $(SLMODEM_DIR)/modem_at.o $(SLMODEM_DIR)/modem_timer.o \
               $(SLMODEM_DIR)/modem_pack.o $(SLMODEM_DIR)/modem_ec.o \
               $(SLMODEM_DIR)/modem_comp.o $(SLMODEM_DIR)/modem_param.o \
               $(SLMODEM_DIR)/modem_debug.o $(SLMODEM_DIR)/homolog_data.o \
               $(SLMODEM_DIR)/dp_sinus.o $(SLMODEM_DIR)/dp_dummy.o \
               $(SLMODEM_DIR)/sysdep_common.o

.PHONY: all clean build_re build_baresip build_slmodem

all: baresip_play rtp_bridge slmodem_bridge pri_call

build_re:
	$(MAKE) -C $(REPO_DIR)/re

build_baresip: build_re
	@if [ ! -d $(REPO_DIR)/baresip/build ]; then \
		cmake -B $(REPO_DIR)/baresip/build -S $(REPO_DIR)/baresip \
			-DRE_INCLUDE_DIRS="$(REPO_DIR)/re/include" \
			-DRE_LIBRARIES="$(REPO_DIR)/re/libre.a" \
			-DSTATIC=ON \
			-DMODULES="account;aufile;g711;stdio"; \
	fi
	cmake --build $(REPO_DIR)/baresip/build -j

baresip_play: baresip_play.o build_baresip
	$(CC) -o $@ baresip_play.o $(LDFLAGS)

baresip_play.o: baresip_play.c
	$(CC) $(CFLAGS) -c $< -o $@

rtp_bridge: rtp_bridge.c build_re
	$(CC) $(CFLAGS) -g -o $@ $< -L$(REPO_DIR)/re/build -Wl,-rpath,'$$ORIGIN/../re/build' -lre -lm -lpthread

build_slmodem:
	$(MAKE) -C $(SLMODEM_DIR)

# SnrToRetrainTable is local in the vendor object.  Export it only in a
# generated bridge-specific copy so slmodem_bridge can tune the local V.32bis
# retrain policy without modifying the original binary.
$(SLMODEM_BRIDGE_DSP_OBJ): $(SLMODEM_DSP_OBJ) | build_slmodem
	$(OBJCOPY) --globalize-symbol=SnrToRetrainTable $< $@

# RcFixed from dsplibs.o is the default low-latency resampler.  Set
# SLMODEM_USE_LIBSAMPLERATE=1 to build the comparison/fallback path.
SLMODEM_USE_LIBSAMPLERATE ?= 0
ifeq ($(SLMODEM_USE_LIBSAMPLERATE),1)
SLM_RESAMPLER_CFLAGS := -DUSE_LIBSAMPLERATE
SLM_RESAMPLER_LIBS := $(shell find /usr/lib/i386-linux-gnu /usr/lib32 -name 'libsamplerate.so*' 2>/dev/null | head -1)
endif

slmodem_bridge: slmodem_bridge.c build_slmodem $(SLMODEM_BRIDGE_DSP_OBJ)
	$(CC) -m32 -Wall -g -O2 $(SLM_RESAMPLER_CFLAGS) -I$(SLMODEM_DIR) \
		-o $@ $< $(SLMODEM_OBJS) $(SLMODEM_BRIDGE_DSP_OBJ) \
		$(SLM_RESAMPLER_LIBS) -lm -lpthread

# pri_call (DAHDI/libpri E1 call + B-channel bridge) needs libpri and re's
# g711.c; it is 64-bit like rtp_bridge/baresip_play (not -m32).
pri_call: pri_call.c
	$(CC) $(CFLAGS) -o $@ pri_call.c $(REPO_DIR)/re/rem/g711/g711.c -lpri

clean:
	$(MAKE) -C $(REPO_DIR)/re clean || true
	$(MAKE) -C $(SLMODEM_DIR) clean || true
	rm -rf $(REPO_DIR)/baresip/build
	rm -f baresip_play.o baresip_play rtp_bridge slmodem_bridge pri_call \
		$(SLMODEM_BRIDGE_DSP_OBJ)
