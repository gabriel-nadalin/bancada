BANCADA_DIR = /home/gabriel/bancada

CC = gcc
CFLAGS = -Wall -Wextra -O2 \
         -I$(BANCADA_DIR)/re/include \
         -I$(BANCADA_DIR)/baresip/include

LDFLAGS = -L$(BANCADA_DIR)/baresip/build \
          -L$(BANCADA_DIR)/re/build \
          -Wl,-rpath,$(BANCADA_DIR)/re/build \
          -lbaresip -lre -lpthread -lm -lssl -lcrypto

SLMODEM_DIR = $(BANCADA_DIR)/slmodem-2.9.11-20110321/modem
SLMODEM_OBJS = $(SLMODEM_DIR)/modem.o $(SLMODEM_DIR)/modem_datafile.o \
               $(SLMODEM_DIR)/modem_at.o $(SLMODEM_DIR)/modem_timer.o \
               $(SLMODEM_DIR)/modem_pack.o $(SLMODEM_DIR)/modem_ec.o \
               $(SLMODEM_DIR)/modem_comp.o $(SLMODEM_DIR)/modem_param.o \
               $(SLMODEM_DIR)/modem_debug.o $(SLMODEM_DIR)/homolog_data.o \
               $(SLMODEM_DIR)/dp_sinus.o $(SLMODEM_DIR)/dp_dummy.o \
               $(SLMODEM_DIR)/sysdep_common.o $(SLMODEM_DIR)/dsplibs.o

.PHONY: all clean build_re build_baresip build_slmodem

all: baresip_play rtp_bridge slmodem_bridge

build_re:
	$(MAKE) -C re

build_baresip: build_re
	@if [ ! -d baresip/build ]; then \
		cmake -B baresip/build -S baresip \
			-DRE_INCLUDE_DIRS="$(BANCADA_DIR)/re/include" \
			-DRE_LIBRARIES="$(BANCADA_DIR)/re/libre.a" \
			-DSTATIC=ON \
			-DMODULES="account;aufile;g711;stdio"; \
	fi
	cmake --build baresip/build -j

baresip_play: baresip_play.o build_baresip
	$(CC) -o $@ baresip_play.o $(LDFLAGS)

baresip_play.o: baresip_play.c
	$(CC) $(CFLAGS) -c $< -o $@

rtp_bridge: rtp_bridge.c build_re
	$(CC) $(CFLAGS) -g -o $@ $< -L$(BANCADA_DIR)/re/build -Wl,-rpath,$(BANCADA_DIR)/re/build -lre -lm -lpthread

build_slmodem:
	$(MAKE) -C $(SLMODEM_DIR)

slmodem_bridge: slmodem_bridge.c build_slmodem
	$(CC) -m32 -Wall -g -O2 -I$(SLMODEM_DIR) -o $@ $< $(SLMODEM_OBJS) \
		/usr/lib/i386-linux-gnu/libsamplerate.so.0 -lm -lpthread

clean:
	$(MAKE) -C re clean || true
	$(MAKE) -C $(SLMODEM_DIR) clean || true
	rm -rf baresip/build
	rm -f baresip_play.o baresip_play rtp_bridge slmodem_bridge
