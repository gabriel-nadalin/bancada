BANCADA_DIR = /home/gabriel/bancada

CC = gcc
CFLAGS = -Wall -Wextra -O2 \
         -I$(BANCADA_DIR)/re/include \
         -I$(BANCADA_DIR)/baresip/include

LDFLAGS = -L$(BANCADA_DIR)/baresip/build \
          -L$(BANCADA_DIR)/re/build \
          -Wl,-rpath,$(BANCADA_DIR)/re/build \
          -lbaresip -lre -lpthread -lm -lssl -lcrypto

.PHONY: all clean build_re build_baresip

all: dial_and_play rtp_bridge

build_re:
	$(MAKE) -C re

build_baresip: build_re
	cmake -B baresip/build -S baresip \
		-DRE_INCLUDE_DIRS="$(BANCADA_DIR)/re/include" \
		-DRE_LIBRARIES="$(BANCADA_DIR)/re/libre.a" \
		-DSTATIC=ON \
		-DMODULES="account;aufile;ausine;g711;stdio" \
		-DAPP_MODULES_DIR=$(BANCADA_DIR)/modules \
		-DAPP_MODULES="auburst;audelay"
	cmake --build baresip/build -j

dial_and_play: dial_and_play.o build_baresip
	$(CC) -o $@ dial_and_play.o $(LDFLAGS)

dial_and_play.o: dial_and_play.c
	$(CC) $(CFLAGS) -c $< -o $@

rtp_bridge: rtp_bridge.c build_re
	$(CC) $(CFLAGS) -g -o $@ $< -L$(BANCADA_DIR)/re/build -Wl,-rpath,$(BANCADA_DIR)/re/build -lre -lm -lpthread

clean:
	$(MAKE) -C re clean || true
	rm -rf baresip/build
	rm -f dial_and_play.o dial_and_play rtp_bridge
