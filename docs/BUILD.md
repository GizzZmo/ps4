# Build Guide — PS4 Mach-O Loader

This guide walks through every supported build configuration: development host (macOS), development host (Linux), and the PS4 SDK toolchain.

---

## Table of Contents

1. [Official Darwin x86 / x64 Download Links](#official-darwin-x86--x64-download-links)
2. [Prerequisites](#prerequisites)
3. [Building the Bare-Metal Payload](#building-the-bare-metal-payload)
   - [macOS (native)](#macos-native)
   - [Linux with osxcross](#linux-with-osxcross)
   - [Verifying the Output](#verifying-the-output)
4. [Building the Loader on a Development Host](#building-the-loader-on-a-development-host)
   - [macOS](#macos)
   - [Linux](#linux)
5. [Building for PS4](#building-for-ps4)
6. [Building the ISO Loader](#building-the-iso-loader)
7. [Optional: Makefile](#optional-makefile)
8. [Build Flags Reference](#build-flags-reference)

---

## Official Darwin x86 / x64 Download Links

The following are the **official** Apple sources for the Darwin / macOS SDK and
toolchain components needed to cross-compile Mach-O x86 and x86_64 binaries.

### macOS SDK and Toolchain

| Resource | Official URL |
|----------|-------------|
| **Xcode** (full IDE + macOS SDK, recommended) | <https://developer.apple.com/download/applications/> |
| **Xcode Command Line Tools** (SDK only, no IDE) | <https://developer.apple.com/download/all/?q=command+line+tools> |
| **Apple Open Source** (Darwin kernel, XNU, libdispatch, …) | <https://opensource.apple.com/> |
| **XNU kernel source** — Apple OSS Distributions on GitHub | <https://github.com/apple-oss-distributions/xnu> |
| **macOS release history and direct downloads** | <https://support.apple.com/en-us/100100> |

> **Intel / x86_64 note:** macOS Sonoma (14) is the last release to support
> Intel (x86_64) Macs.  The `-target x86_64-apple-macos` Clang flag produces
> x86_64 Mach-O binaries from any macOS host (Intel or Apple Silicon) and from
> Linux with the osxcross toolchain.

### Linux Cross-Compilation

| Resource | Official URL |
|----------|-------------|
| **osxcross** — macOS cross-compilation toolchain for Linux | <https://github.com/tpoechtrager/osxcross> |
| **LLVM / Clang releases** (required by osxcross) | <https://releases.llvm.org/> |
| **Apple Open Source tarballs** (SDK header tarballs used by osxcross) | <https://opensource.apple.com/tarballs/> |

---

## Prerequisites

### macOS

| Tool | Install |
|------|---------|
| Xcode Command Line Tools | `xcode-select --install` or download from <https://developer.apple.com/download/all/?q=command+line+tools> |
| Clang ≥ 10 (bundled with Xcode) | Included with Xcode CLT |
| `otool` | Included with Xcode CLT |

### Linux

| Tool | Install |
|------|---------|
| Clang ≥ 10 | `apt install clang` / `dnf install clang` |
| `objdump` | `apt install binutils` |
| `osxcross` (for payload cross-compilation) | See [osxcross setup](#linux-with-osxcross) |

### PS4 SDK

- A working PS4 homebrew toolchain (e.g. the OpenOrbis toolchain).
- The `x86_64-ps4-clang` (or equivalent) wrapper script on `PATH`.

---

## Building the Bare-Metal Payload

`payload.c` must be cross-compiled into a **Mach-O** binary. The resulting file is what the loader parses at runtime.

### macOS (native)

```bash
clang -target x86_64-apple-macos \
      -static \
      -nostdlib \
      -e __start \
      -o bare_metal_test payload.c
```

Flag explanations:

| Flag | Effect |
|------|--------|
| `-target x86_64-apple-macos` | Produce a 64-bit Mach-O for macOS (no iOS/ARM) |
| `-static` | Do not link against any dynamic libraries |
| `-nostdlib` | Do not link `libSystem`, `libc`, or CRT startup code |
| `-e __start` | Set the Mach-O entry point to `__start` (note: two underscores — Darwin prepends one to every C symbol) |

### Linux with osxcross

[osxcross](https://github.com/tpoechtrager/osxcross) provides a complete macOS cross-compilation environment on Linux.

**One-time osxcross setup (summary):**

```bash
git clone https://github.com/tpoechtrager/osxcross.git
cd osxcross
# Place a macOS SDK tarball in tarballs/ — see osxcross README for details
UNATTENDED=1 ./build.sh
export PATH="$PATH:$(pwd)/target/bin"
```

**Build the payload:**

```bash
o64-clang -target x86_64-apple-macos \
          -static \
          -nostdlib \
          -e __start \
          -o bare_metal_test payload.c
```

> `o64-clang` is the osxcross wrapper for 64-bit macOS targets.

### Verifying the Output

Always inspect the generated binary before loading it into the PS4 environment.

**macOS:**

```bash
# Confirm it is a Mach-O 64-bit executable
file bare_metal_test

# Inspect load commands
otool -l bare_metal_test

# Confirm the entry-point symbol
nm -a bare_metal_test | grep start
```

**Linux (with binutils or llvm-tools):**

```bash
file bare_metal_test
objdump -p bare_metal_test    # load commands
nm bare_metal_test | grep start
```

**What to look for:**

```
Load command N
      cmd LC_SEGMENT_64
  cmdsize …
  segname __TEXT
   vmaddr 0x0000000100000000
   vmsize 0x0000000000001000
  fileoff 0
 filesize …
  maxprot 0x00000005          ← R-X
 initprot 0x00000005          ← R-X
   nsects 1
    flags 0x0

Load command M
        cmd LC_MAIN
    cmdsize 24
    entryoff …                ← must be non-zero (offset to _start)
   stacksize 0
```

---

## Building the Loader on a Development Host

The loader itself (`macho_loader.c` + `macho_test.c`) is standard C99/C11 POSIX code and compiles without any extra flags on macOS or Linux.

### macOS

```bash
clang -std=c11 -Wall -Wextra -O2 \
      -o macho_loader_test \
      macho_loader.c macho_test.c \
      -I.
```

### Linux

```bash
clang -std=c11 -Wall -Wextra -O2 \
      -o macho_loader_test \
      macho_loader.c macho_test.c \
      -I.
```

Or with GCC:

```bash
gcc -std=c11 -Wall -Wextra -O2 \
    -o macho_loader_test \
    macho_loader.c macho_test.c \
    -I.
```

> `MAP_FIXED_NOREPLACE` is available on Linux ≥ 4.17 and is automatically selected at compile time via the `#ifdef` guard in `macho_loader.c`. On older kernels the loader silently falls back to a non-fixed mapping.

---

## Building for PS4

The PS4 userland uses a FreeBSD-derived libc. The only extra step is:

1. Use the PS4 toolchain compiler (e.g. `x86_64-ps4-clang`).
2. Define `SCE_KERNEL_MPROTECT_AVAILABLE` so the `ps4_mprotect` macro resolves to `sceKernelMprotect` instead of POSIX `mprotect`.

```bash
PS4CC=x86_64-ps4-clang   # adjust to match your toolchain

${PS4CC} -std=c11 -O2 \
    -DSCE_KERNEL_MPROTECT_AVAILABLE \
    -c macho_loader.c -o macho_loader.o

${PS4CC} -std=c11 -O2 \
    -DSCE_KERNEL_MPROTECT_AVAILABLE \
    -c macho_test.c -o macho_test.o
```

Link the resulting object files into your existing PS4 payload project as usual. See [`PS4_INTEGRATION.md`](PS4_INTEGRATION.md) for the full integration walkthrough.

---

## Building the ISO Loader

The ISO loader (`iso_loader.c`) is standard C11 POSIX code.  It compiles on any
POSIX host or the PS4 SDK with no extra flags.

### Development host (macOS / Linux)

```bash
# Build the ISO loader and run its self-contained test:
make iso_loader
./iso_loader_test
# Expected: ISO loader test: PASS
```

Or manually:

```bash
clang -std=c11 -Wall -Wextra -O2 -I. \
      -o iso_loader_test iso_loader.c iso_loader_test.c
```

### PS4 SDK

```bash
PS4CC=x86_64-ps4-clang

${PS4CC} -std=c11 -O2 -I. \
    -DSCE_KERNEL_MPROTECT_AVAILABLE \
    -c iso_loader.c -o iso_loader.o
```

Link `iso_loader.o` into your existing PS4 payload.  See
[`ISO_LOADER.md`](ISO_LOADER.md) for the complete PS4 integration guide.

---

## Optional: Makefile

A minimal `Makefile` for the development host:

```makefile
CC      := clang
CFLAGS  := -std=c11 -Wall -Wextra -O2 -I.

PAYLOAD_CC    := clang
PAYLOAD_FLAGS := -target x86_64-apple-macos -static -nostdlib -e __start

all: bare_metal_test macho_loader_test

bare_metal_test: payload.c
	$(PAYLOAD_CC) $(PAYLOAD_FLAGS) -o $@ $<

macho_loader_test: macho_loader.c macho_test.c
	$(CC) $(CFLAGS) -o $@ $^

clean:
	rm -f bare_metal_test macho_loader_test
```

---

## Build Flags Reference

| Flag | Component | Purpose |
|------|-----------|---------|
| `-target x86_64-apple-macos` | payload | Produce a 64-bit Mach-O; required for cross-compilation on Linux |
| `-static` | payload | No dynamic library dependencies |
| `-nostdlib` | payload | Exclude libc and CRT startup code |
| `-e __start` | payload | Override entry point to `__start` (C `_start` + Darwin underscore prefix) |
| `-DSCE_KERNEL_MPROTECT_AVAILABLE` | loader | Switch `ps4_mprotect` to `sceKernelMprotect` |
| `-std=c11` | loader | Enable C11 for `<stdint.h>` fixed-width types and inline `memset`/`memcpy` |
| `-Wall -Wextra` | loader | Enable broad warning set during development |
| `-O2` | loader | Optimize; avoids accidental UB from unoptimised pointer arithmetic |
