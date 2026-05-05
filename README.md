# PS4 Mach-O Loader — Proof of Concept

[![Build Loader](https://github.com/GizzZmo/ps4/actions/workflows/build-loader.yml/badge.svg)](https://github.com/GizzZmo/ps4/actions/workflows/build-loader.yml)
[![Build Payload](https://github.com/GizzZmo/ps4/actions/workflows/build-payload.yml/badge.svg)](https://github.com/GizzZmo/ps4/actions/workflows/build-payload.yml)
[![Release](https://github.com/GizzZmo/ps4/actions/workflows/release.yml/badge.svg)](https://github.com/GizzZmo/ps4/actions/workflows/release.yml)

> **Use at your own risk.** This project is for educational and research purposes only. Running unsigned code on a retail PS4 may void your warranty and violate Sony's terms of service.

A self-contained, portable Mach-O 64-bit segment loader written in C that runs on the PS4's FreeBSD-based kernel. It parses a raw Mach-O image, maps each `LC_SEGMENT_64` into anonymous memory, and hands off execution to the binary's entry point — without depending on any Darwin or macOS system libraries.

---

## Table of Contents

1. [Overview](#overview)
2. [Repository Structure](#repository-structure)
3. [How It Works](#how-it-works)
4. [Quick Start](#quick-start)
5. [Building the Payload](#building-the-payload)
6. [Building the Loader](#building-the-loader)
7. [Running the Test](#running-the-test)
8. [PS4 Integration](#ps4-integration)
9. [ISO Loader — Installing an OS from Disc Image](#iso-loader--installing-an-os-from-disc-image)
10. [Official Darwin x86 / x64 Resources](#official-darwin-x86--x64-resources)
11. [API Reference](#api-reference)
12. [Security Notes](#security-notes)
13. [Troubleshooting](#troubleshooting)
14. [Further Reading](#further-reading)
15. [Science and Art](#science-and-art)
16. [About](#about)

---

## Overview

The PS4 runs a customised version of FreeBSD 9 on an x86-64 CPU. Apple's macOS uses the **Mach-O** object format for all executables. Because both platforms share the same CPU architecture (AMD64), a Mach-O binary compiled without any OS-specific dependencies can be *mapped and executed* on the PS4 by reimplementing the small subset of the Mach-O loader that the kernel normally handles.

This project provides:

| Component | File(s) | Purpose |
|-----------|---------|---------|
| Type definitions & public API | `macho_loader.h` | Self-contained Mach-O structs; no `<mach-o/loader.h>` needed |
| Segment loader | `macho_loader.c` | Parses the image and maps segments into anonymous memory |
| Execution harness | `macho_test.c` | Promotes pages to RX, issues a memory fence, and runs the payload |
| Driver / entry point | `main.c` | Reads the payload file and calls `test_macho_execution` |
| Bare-metal payload | `payload.c` | Minimal `_start` with no libc – cross-compiled to Mach-O |
| ISO 9660 / El Torito loader | `iso_loader.h`, `iso_loader.c` | Parses disc images and extracts boot images or individual files |

---

## Repository Structure

```
ps4/
├── macho_loader.h   # Mach-O type definitions and public API declarations
├── macho_loader.c   # Core segment loader (load_mach_o_segments)
├── macho_test.c     # Execution harness (test_macho_execution)
├── main.c           # Driver: reads payload file, calls test_macho_execution
├── payload.c        # Bare-metal payload source (cross-compiled separately)
├── iso_loader.h     # ISO 9660 / El Torito type definitions and public API
├── iso_loader.c     # ISO loader (iso_load_boot_image, iso_find_file)
├── iso_loader_test.c# Self-contained ISO loader test (no disc file needed)
├── Makefile         # Build rules for loader, payload, and ISO loader
├── docs/
│   ├── ARCHITECTURE.md      # Deep dive into loader internals
│   ├── BUILD.md             # Step-by-step build instructions
│   ├── ISO_LOADER.md        # ISO 9660 / El Torito loader guide
│   ├── API_REFERENCE.md     # Full public API reference
│   ├── PS4_INTEGRATION.md   # PS4-specific how-to guide
│   └── TROUBLESHOOTING.md   # Common issues & solutions
├── CONTRIBUTING.md
└── LICENSE
```

---

## How It Works

The loader performs **three sequential passes** over the Mach-O image held in memory:

```
┌─────────────────────────────────────────────────────────┐
│  Pass 1 – Measure                                       │
│  Walk every LC_SEGMENT_64 and record the lowest vmaddr  │
│  and the highest (vmaddr + vmsize) to determine the     │
│  total virtual-address span.                            │
└───────────────────────┬─────────────────────────────────┘
                        │
                        ▼
┌─────────────────────────────────────────────────────────┐
│  Single mmap() reservation                              │
│  One contiguous RW region covers the entire span.       │
│  An ASLR slide is computed: slide = base − vm_min.      │
└───────────────────────┬─────────────────────────────────┘
                        │
                        ▼
┌─────────────────────────────────────────────────────────┐
│  Pass 2 – Copy                                          │
│  Each segment's raw bytes are memcpy'd from the file    │
│  image into the mapped region at (vmaddr − vm_min).     │
└───────────────────────┬─────────────────────────────────┘
                        │
                        ▼
┌─────────────────────────────────────────────────────────┐
│  Pass 3 – Locate entry point                            │
│  LC_MAIN  → entry_addr = base + entryoff                │
│  LC_UNIXTHREAD → entry_addr = rip_from_thread_state     │
│                              + slide                    │
└───────────────────────┬─────────────────────────────────┘
                        │
                        ▼
┌─────────────────────────────────────────────────────────┐
│  Caller (macho_test.c)                                  │
│  mprotect(RW→RX) → mfence → call entry_addr()          │
└─────────────────────────────────────────────────────────┘
```

See [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) for a full deep-dive.

---

## Quick Start

### Prerequisites

| Tool | Minimum version | Purpose |
|------|----------------|---------|
| Clang | 10+ | Compile loader & cross-compile payload |
| `otool` (macOS) or `objdump` (Linux) | any | Inspect generated Mach-O headers |
| GNU Make (optional) | 3.8+ | Drive the build |

### 1. Clone and build

```bash
git clone https://github.com/GizzZmo/ps4.git
cd ps4

# Build the loader and test harness
make loader
```

### 2. Build the bare-metal payload (macOS or Linux with Clang cross-toolchain)

```bash
clang -target x86_64-apple-macos \
      -static -nostdlib \
      -e __start \
      -o bare_metal_test payload.c
```

### 3. Verify the Mach-O headers

```bash
# macOS
otool -l bare_metal_test

# Linux
objdump -p bare_metal_test
```

Look for `LC_SEGMENT_64` with `vmaddr = 0x100000000` and `LC_MAIN` with a valid `entryoff`.

---

## Building the Payload

`payload.c` is a deliberately dependency-free C file. It must be cross-compiled into a native Mach-O binary so the loader has something to parse.

### macOS / Linux (Clang ≥ 10 with cross-compilation support)

```bash
clang -target x86_64-apple-macos \
      -static    \     # no dynamic linking
      -nostdlib  \     # no libSystem or libc
      -e __start \     # explicit entry-point symbol (two underscores on Darwin)
      -o bare_metal_test payload.c
```

> **Symbol naming note:** Darwin/macOS toolchains automatically prepend an underscore to every C symbol name. The function `_start` in C therefore becomes `__start` in the Mach-O symbol table, which is why `-e __start` (two underscores) must be passed to the linker.

### Expected output

```
$ otool -l bare_metal_test | grep -A4 LC_MAIN
          cmd LC_MAIN
      cmdsize 24
      entryoff 0
     stacksize 0
```

The `entryoff` will be the file offset of `_start` inside the `__TEXT` segment.

---

## Building the Loader

The loader itself (`macho_loader.c` + `macho_test.c`) is standard C99/C11 and compiles on any POSIX host or the PS4 SDK.

### Development host (macOS / Linux)

```bash
clang -std=c11 -Wall -Wextra -O2 \
      -o macho_loader_test \
      macho_loader.c macho_test.c \
      -I.
```

### PS4 SDK (inside the PS4 userland payload project)

```bash
# Replace <PS4_SDK> with the path to your PS4 SDK toolchain
<PS4_SDK>/bin/x86_64-ps4-clang -std=c11 -O2 \
    -DSCE_KERNEL_MPROTECT_AVAILABLE \
    -o macho_loader.o -c macho_loader.c

<PS4_SDK>/bin/x86_64-ps4-clang -std=c11 -O2 \
    -DSCE_KERNEL_MPROTECT_AVAILABLE \
    -o macho_test.o -c macho_test.c
```

Setting `-DSCE_KERNEL_MPROTECT_AVAILABLE` switches the `ps4_mprotect` macro to call `sceKernelMprotect()` instead of the POSIX `mprotect(2)`.

---

## Running the Test

Once both binaries are built, embed `bare_metal_test` as a byte array and pass it to `test_macho_execution`:

```c
#include "macho_loader.h"
#include <stdio.h>
#include <stdlib.h>

/* Read the Mach-O binary produced by payload.c into a buffer. */
static uint8_t *read_file(const char *path, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    *out_size = (size_t)ftell(f);
    rewind(f);
    uint8_t *buf = malloc(*out_size);
    fread(buf, 1, *out_size, f);
    fclose(f);
    return buf;
}

int main(void) {
    size_t  size;
    uint8_t *data = read_file("bare_metal_test", &size);
    if (!data) { fputs("Cannot open bare_metal_test\n", stderr); return 1; }

    int ok = test_macho_execution(data, size);
    printf("Result: %s (expected return value 0x1379)\n",
           ok ? "PASS" : "FAIL");
    free(data);
    return ok ? 0 : 1;
}
```

**Expected output:**

```
Result: PASS (expected return value 0x1379)
```

`0x1379` = `0x1337 + 0x42` — the value returned by `_start()` in `payload.c`.

---

## PS4 Integration

See [`docs/PS4_INTEGRATION.md`](docs/PS4_INTEGRATION.md) for the complete guide. A brief summary:

1. **Obtain a kernel exploit** that allows unsigned code execution (jailbreak). This loader does *not* provide one.
2. **Compile** `macho_loader.c` and `macho_test.c` with the PS4 SDK toolchain and `-DSCE_KERNEL_MPROTECT_AVAILABLE`.
3. **Embed** the Mach-O payload binary inside your existing PS4 payload as a byte array.
4. **Call** `test_macho_execution(payload_bytes, payload_size)` from your payload's entry point.
5. **Verify** the return value; `1` means the Mach-O image was successfully mapped and executed.

### Memory protection on PS4

`mmap()` on PS4 returns pages that are `RW` only. Before jumping to the entry point you **must** upgrade the protection:

```c
// PS4 SDK equivalent of mprotect(2)
sceKernelMprotect(mmap_base, total_size, VM_PROT_READ | VM_PROT_EXEC);
```

The `ps4_mprotect` macro in `macho_test.c` handles this automatically when compiled with `-DSCE_KERNEL_MPROTECT_AVAILABLE`.

---

## ISO Loader — Installing an OS from Disc Image

The ISO loader (`iso_loader.h` / `iso_loader.c`) provides the foundation for
installing or booting a third-party operating system on the PS4 directly from a
standard `.iso` disc image (any Linux distribution, Darwin installer, etc.).

See [`docs/ISO_LOADER.md`](docs/ISO_LOADER.md) for the complete guide. A brief summary:

### What it does

| Function | Purpose |
|----------|---------|
| `iso_load_boot_image(iso_data, iso_size, &out)` | Parse the El Torito boot catalog and extract the default boot image into a `malloc`'d buffer |
| `iso_find_file(iso_data, iso_size, "/path/to/file", &size)` | Traverse the ISO 9660 directory tree and return a direct pointer to any file (kernel, initrd, config) |
| `iso_free_boot_image(&img)` | Release memory allocated by `iso_load_boot_image` |

### Build and test

```bash
make iso_loader       # compile iso_loader_test
./iso_loader_test     # expected: ISO loader test: PASS

make test             # runs both the Mach-O integration test and ISO loader test
```

### Typical PS4 integration flow

```c
#include "iso_loader.h"
#include "macho_loader.h"

/* 1. Read or embed the ISO image */
extern const uint8_t iso_data[];
extern const size_t  iso_size;

/* 2. Extract the El Torito boot image */
iso_boot_image_t boot_img;
if (iso_load_boot_image(iso_data, iso_size, &boot_img) != 0) {
    notify("ISO parse failed");
    return;
}

/* 3a. Darwin boot image — load via the Mach-O loader */
int ok = test_macho_execution(boot_img.data, boot_img.size);
iso_free_boot_image(&boot_img);

/* 3b. Linux ISO — find kernel and initrd directly */
size_t         kernel_size, initrd_size;
const uint8_t *kernel = iso_find_file(iso_data, iso_size,
                                       "/boot/vmlinuz", &kernel_size);
const uint8_t *initrd = iso_find_file(iso_data, iso_size,
                                       "/boot/initrd.img", &initrd_size);
/* Then set up struct boot_params and jump to kernel_base + 0x200 */
```

---

## Official Darwin x86 / x64 Resources

To cross-compile Mach-O x86 and x86_64 binaries targeting Darwin / macOS:

### macOS (native)

| Resource | URL |
|----------|-----|
| Xcode (full IDE + SDK) | <https://developer.apple.com/download/applications/> |
| Xcode Command Line Tools (SDK only) | <https://developer.apple.com/download/all/?q=command+line+tools> |
| Apple Open Source (XNU / Darwin kernel) | <https://opensource.apple.com/> |
| XNU kernel source — GitHub mirror | <https://github.com/apple-oss-distributions/xnu> |
| macOS release history and downloads | <https://support.apple.com/en-us/100100> |

> **Intel / x86_64 note:** macOS Sonoma (14) is the last release to support
> Intel Macs.  The `-target x86_64-apple-macos` Clang flag works from both
> Intel and Apple Silicon hosts and targets the same x86_64 Mach-O ABI.

### Linux (cross-compilation)

| Resource | URL |
|----------|-----|
| osxcross — macOS cross-toolchain for Linux | <https://github.com/tpoechtrager/osxcross> |
| LLVM / Clang releases | <https://releases.llvm.org/> |
| Apple Open Source tarballs (SDK headers) | <https://opensource.apple.com/tarballs/> |

```bash
# Quick osxcross setup
git clone https://github.com/tpoechtrager/osxcross.git
cd osxcross
# Place a macOS SDK tarball in tarballs/ — see the osxcross README
UNATTENDED=1 ./build.sh
export PATH="$PATH:$(pwd)/target/bin"

# Cross-compile Mach-O x86_64 on Linux:
o64-clang -target x86_64-apple-macos -static -nostdlib -e __start \
          -o bare_metal_test payload.c
```

---

## API Reference

See [`docs/API_REFERENCE.md`](docs/API_REFERENCE.md) for the full reference. Key functions:

### `load_mach_o_segments`

```c
void *load_mach_o_segments(const uint8_t *macho_data, size_t size,
                            void **out_map_base, size_t *out_map_size);
```

| Parameter | Description |
|-----------|-------------|
| `macho_data` | Pointer to the raw Mach-O image in memory |
| `size` | Byte length of the buffer |
| `out_map_base` | Optional — receives the `mmap` reservation base (for `munmap`/`mprotect`) |
| `out_map_size` | Optional — receives the `mmap` reservation size (for `munmap`/`mprotect`) |

**Returns** a pointer to the entry point on success, `NULL` on any error (bad magic, missing entry point, allocation failure, malformed load commands).

**Caller responsibilities:**
- Call `mprotect` (or `sceKernelMprotect`) with `out_map_base`/`out_map_size` to set the mapped pages to `RX` before executing the returned pointer.
- Free the mapping with `munmap(out_map_base, out_map_size)` when finished.

---

### `test_macho_execution`

```c
int test_macho_execution(const uint8_t *macho_data, size_t size);
```

Convenience wrapper that calls `load_mach_o_segments`, promotes pages to `RX`, issues `mfence`, executes the payload, and validates the result against `0x1379`.

**Returns** `1` on success (payload returned `0x1379`), `0` on any failure.

---

## Security Notes

- **NX / DEP bypass**: Mapped pages are initially `RW`. The explicit `mprotect(RX)` call removes write permission before execution. This is correct and required; do *not* map pages as `RWX` simultaneously.
- **Buffer validation**: Both passes in `load_mach_o_segments` validate that every load command and segment fits within the supplied buffer before accessing it.
- **ASLR**: The loader uses `MAP_FIXED_NOREPLACE` when available (Linux ≥ 4.17), falling back to a kernel-chosen address. The ASLR slide is correctly propagated to `LC_UNIXTHREAD` entry points.
- **No syscall translation**: This PoC only handles the binary format. Any payload that makes Darwin-specific syscalls will crash on PS4 until a Darwin→FreeBSD syscall shim is added.

---

## Troubleshooting

| Symptom | Likely cause | Fix |
|---------|-------------|-----|
| `load_mach_o_segments` returns `NULL` | Wrong magic bytes / not a 64-bit Mach-O | Confirm file with `file bare_metal_test` |
| Segmentation fault on execution | Pages not promoted to RX | Ensure `mprotect`/`sceKernelMprotect` succeeds before the jump |
| Result is not `0x1379` | Symbol name mismatch | Verify with `nm bare_metal_test`; entry must be `__start` |
| `mmap` fails on PS4 | Insufficient RW region | Ensure the kernel exploit has unlocked user-land memory mapping |
| Clang cross-compile fails on Linux | Missing macOS SDK sysroot | Use `osxcross` or a macOS VM; see `docs/BUILD.md` |

See [`docs/TROUBLESHOOTING.md`](docs/TROUBLESHOOTING.md) for an extended list with stack traces and diagnostic commands.

---

## Further Reading

- [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) — Mach-O format primer, loader design, ASLR handling
- [`docs/BUILD.md`](docs/BUILD.md) — Detailed build matrix (macOS, Linux, PS4 SDK)
- [`docs/ISO_LOADER.md`](docs/ISO_LOADER.md) — ISO 9660 / El Torito loader guide and PS4 OS installation
- [`docs/API_REFERENCE.md`](docs/API_REFERENCE.md) — Full API documentation with error codes
- [`docs/PS4_INTEGRATION.md`](docs/PS4_INTEGRATION.md) — End-to-end PS4 integration walkthrough
- [`docs/TROUBLESHOOTING.md`](docs/TROUBLESHOOTING.md) — Diagnostics and common pitfalls
- [Apple's Mach-O Runtime Architecture](https://developer.apple.com/library/archive/documentation/DeveloperTools/Conceptual/MachORuntime/index.html) — Official format specification
- [Apple Open Source (Darwin / XNU)](https://opensource.apple.com/) — Official Darwin kernel and toolchain source
- [XNU kernel source — GitHub mirror](https://github.com/apple-oss-distributions/xnu) — Latest Darwin x86/x64 kernel code
- [PS4 Developer Wiki](https://www.psdevwiki.com/ps4/) — Community PS4 internals reference

---

## License

See [LICENSE](LICENSE) for terms.

---

## Science and Art

This project sits at the intersection of **low-level systems engineering** and the craft of understanding how hardware and software interact at a fundamental level.

### Science

- **Binary format research** — dissecting the Mach-O object format to understand how modern operating systems load and execute code, from ELF on Linux to Mach-O on macOS/Darwin.
- **Cross-architecture portability** — exploring how the same AMD64 instruction stream can be executed on wildly different OS kernels (FreeBSD/PS4 vs. macOS) given the right loader shim.
- **Memory management** — studying virtual memory mapping, page permissions (`RW → RX`), ASLR, and the kernel interfaces (`mmap`, `mprotect`, `sceKernelMprotect`) that control them.
- **Reverse engineering** — using tools such as `otool`, `objdump`, and `nm` to inspect and validate binary artefacts — a core skill in security research and embedded systems.

### Art

- **Minimal C as craft** — writing dependency-free C that compiles cleanly with `-nostdlib` is a discipline: every byte must be intentional, every pointer validated.
- **Systems as canvas** — hacking a game console to run foreign binaries is, in its own way, a creative act: turning a closed device into an open platform where new ideas can run.
- **Documentation as storytelling** — the ASCII flowcharts, tables, and structured prose in this project reflect the belief that clear communication is as important as correct code.

---

## About

### Jon-Arve Constantine

**Jon-Arve Constantine** is a developer and researcher with a passion for low-level systems programming, security research, and open-source exploration.

- 🐙 GitHub: [github.com/GizzZmo](https://www.github.com/GizzZmo)

### About This Project

**PS4 Mach-O Loader** is a proof-of-concept that demonstrates how a Mach-O 64-bit binary — normally only executable on macOS/Darwin — can be mapped and run on the PS4's FreeBSD-based kernel. It is intended purely for educational and research purposes, showing how binary loaders work at the OS level and how CPU-architecture compatibility can be leveraged across different operating systems.

Key goals:
- Understand the Mach-O load-command pipeline end-to-end.
- Produce a minimal, auditable loader with no external dependencies.
- Serve as a foundation for further syscall-shim research.

### Other Projects

Explore more work by Jon-Arve Constantine at **[github.com/GizzZmo](https://www.github.com/GizzZmo)**, including projects spanning systems programming, security tooling, and open-source experiments.
