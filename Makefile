CC            := clang
CFLAGS        := -std=c11 -Wall -Wextra -O2 -I.

PAYLOAD_CC    := clang
PAYLOAD_FLAGS := -target x86_64-apple-macos -static -nostdlib -e __start

.PHONY: all clean loader payload test iso_loader

all: loader payload iso_loader

# Build the loader + execution harness (Linux / macOS development host)
loader: macho_loader_test

macho_loader_test: macho_loader.c macho_test.c main.c macho_loader.h
	$(CC) $(CFLAGS) -o $@ macho_loader.c macho_test.c main.c

# Cross-compile the bare-metal Mach-O payload
# Requires Clang with macOS cross-compilation support (native on macOS;
# on Linux install osxcross and set PAYLOAD_CC=o64-clang).
payload: bare_metal_test

bare_metal_test: payload.c
	$(PAYLOAD_CC) $(PAYLOAD_FLAGS) -o $@ $<

# Self-contained integration test – embeds the Mach-O as a C byte array so
# no cross-compilation toolchain is needed.
integration_test: macho_loader.c macho_test.c integration_test.c \
                  macho_loader.h bare_metal_test_data.h
	$(CC) $(CFLAGS) -o $@ macho_loader.c macho_test.c integration_test.c

# ISO 9660 / El Torito loader and its self-contained test.
# The test constructs a minimal disc image in memory — no ISO file needed.
iso_loader: iso_loader_test

iso_loader_test: iso_loader.c iso_loader_test.c iso_loader.h
	$(CC) $(CFLAGS) -o $@ iso_loader.c iso_loader_test.c

# Run both the Mach-O integration test and the ISO loader test.
test: integration_test iso_loader_test
	./integration_test
	./iso_loader_test

clean:
	rm -f macho_loader_test bare_metal_test integration_test iso_loader_test *.o
