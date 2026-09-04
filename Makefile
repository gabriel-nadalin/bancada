# Top-level build and external dependency preparation.

SLMODEM_VERSION := 2.9.11-20110321
SLMODEM_DIR := slmodem-$(SLMODEM_VERSION)
SLMODEM_ARCHIVE := $(SLMODEM_DIR).tar.gz
SLMODEM_URL := https://deb.debian.org/debian/pool/non-free/s/sl-modem/sl-modem_2.9.11~20110321.orig.tar.gz
SLMODEM_SHA256 := cea496e9c34a16cf347124b27ed673f3a6e87089f83e15ab5e0b0365a56fd253
SLMODEM_PATCH := patches/slmodem-sysmacros.patch
SLMODEM_STAMP := $(SLMODEM_DIR)/.bench-prepared

.PHONY: all clean dependencies re baresip slmodem analysis-v90 report

all: dependencies
	$(MAKE) -C tools

dependencies: re baresip slmodem

re baresip:
	git submodule update --init -- $@

slmodem: $(SLMODEM_STAMP)

$(SLMODEM_ARCHIVE):
	curl --fail --location --output $@ $(SLMODEM_URL)

$(SLMODEM_STAMP): $(SLMODEM_ARCHIVE) $(SLMODEM_PATCH)
	printf '%s  %s\n' '$(SLMODEM_SHA256)' '$(SLMODEM_ARCHIVE)' | sha256sum --check -
	tar -xzf $(SLMODEM_ARCHIVE)
	patch --directory=$(SLMODEM_DIR) --strip=1 < $(SLMODEM_PATCH)
	touch $@

clean:
	$(MAKE) -C tools clean

analysis-v90:
	$(MAKE) -C analysis/v90_sip

report: analysis-v90
	cd docs && latexmk -lualatex -interaction=nonstopmode -halt-on-error relatorio.tex
