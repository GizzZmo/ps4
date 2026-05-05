CC            := clang
CFLAGS        := -std=c11 -Wall -Wextra -O2 -I.

PAYLOAD_CC    := clang
PAYLOAD_FLAGS := -target x86_64-apple-macos -static -nostdlib -e __start

.PHONY: all clean loader payload

all: loader payload

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

clean:
	rm -f macho_loader_test bare_metal_test *.o
