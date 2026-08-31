# Top-level build: delegate to tools/ (the C transport tools).

.PHONY: all clean

all:
	$(MAKE) -C tools

clean:
	$(MAKE) -C tools clean
