# ISO Loader — PS4 OS Installation from Disc Image

This document describes the ISO 9660 / El Torito loader that forms the foundation
for installing or booting a third-party operating system on the PS4 directly from
a standard disc image (`.iso` file).

---

## Table of Contents

1. [Purpose](#purpose)
2. [Background](#background)
3. [Official Darwin x86 / x64 Resources](#official-darwin-x86--x64-resources)
4. [Architecture](#architecture)
5. [API Reference](#api-reference)
6. [Quick Start](#quick-start)
7. [PS4 Integration](#ps4-integration)
8. [Testing](#testing)
9. [Limitations and Next Steps](#limitations-and-next-steps)

---

## Purpose

The PS4 Mach-O loader already demonstrates how to map and execute a foreign binary
on the PS4's FreeBSD-based kernel.  The natural next step is to boot a fully
featured operating system — for example a Linux distribution — from a standard
`.iso` disc image.

This module provides two building blocks:

| Function | Purpose |
|----------|---------|
| `iso_load_boot_image()` | Parse an El Torito boot catalog and extract the default boot image |
| `iso_find_file()` | Traverse the ISO 9660 directory tree and return a pointer to any file (kernel, initrd, config) |

Together they allow a PS4 payload to:

1. Read a disc image from a USB drive, network socket, or embedded byte array.
2. Extract the El Torito boot image (BIOS or UEFI boot loader).
3. Optionally extract individual files such as `vmlinuz` and `initrd.img`.
4. Load and execute the boot loader / kernel using the existing Mach-O loader
   infrastructure.

---

## Background

### ISO 9660

ISO 9660 is the standard filesystem for CD-ROM / DVD / Blu-ray disc images.
Every modern Linux distribution and macOS installer ships as an ISO 9660 image.

Key concepts used by this loader:

| Concept | Detail |
|---------|--------|
| Sector size | 2048 bytes per logical block |
| System area | Sectors 0–15 are reserved (unused by ISO 9660) |
| Volume Descriptors | Start at sector 16; one descriptor per sector |
| Primary VD | Type 1; contains the root directory record |
| VD Set Terminator | Type 255; ends the descriptor sequence |
| Directory Record | Variable-length entry; name is stored in upper case |
| Version suffix | Mastering tools append `;1` to file names; stripped by this loader |

### El Torito

El Torito is a boot extension to ISO 9660.  It allows a disc image to carry
a boot loader that a BIOS or UEFI firmware can execute directly.

| El Torito concept | Detail |
|-------------------|--------|
| Boot Record VD | Type 0; contains the LBA of the boot catalog |
| Boot catalog | One sector; starts with a validation entry, then boot entries |
| Validation entry | 32 bytes; `header_id = 0x01`, key `{ 0x55, 0xAA }` |
| Initial/Default Entry | 32 bytes; `boot_indicator = 0x88` means bootable |
| `load_rba` | LBA of the boot image on the disc |
| `sector_count` | Size of the boot image in 512-byte virtual sectors |
| Media type | 0 = no-emulation (all modern ISOs); 1–3 = floppy emulation; 4 = HD |
| Platform ID | 0x00 = x86 BIOS; 0xEF = UEFI (EFI) |

---

## Official Darwin x86 / x64 Resources

To cross-compile Mach-O x86 and x86_64 binaries targeting Darwin / macOS:

### macOS (native)

| Resource | URL |
|----------|-----|
| Xcode (full IDE + SDK) | <https://developer.apple.com/download/applications/> |
| Xcode Command Line Tools (SDK only, no IDE) | <https://developer.apple.com/download/all/?q=command+line+tools> |
| Apple Open Source (XNU, libdispatch, …) | <https://opensource.apple.com/> |
| XNU kernel source (GitHub mirror) | <https://github.com/apple-oss-distributions/xnu> |
| macOS release history and downloads | <https://support.apple.com/en-us/100100> |

> **Note:** Intel (x86_64) Macs are supported through macOS Sonoma (14).
> macOS Sequoia (15) and later require Apple Silicon.  macOS Tahoe 26.4.1 is
> the latest release and requires Apple Silicon.  The
> `-target x86_64-apple-macos` cross-compilation flag works from both
> Intel and Apple Silicon hosts.

### Linux (cross-compilation)

| Resource | URL |
|----------|-----|
| osxcross — complete macOS cross-compilation toolchain for Linux | <https://github.com/tpoechtrager/osxcross> |
| LLVM / Clang releases (required by osxcross) | <https://releases.llvm.org/> |
| Apple Open Source tarballs (for SDK headers) | <https://opensource.apple.com/tarballs/> |

Quick setup with osxcross:

```bash
git clone https://github.com/tpoechtrager/osxcross.git
cd osxcross
# Place a macOS SDK tarball in tarballs/ — see the osxcross README for details
UNATTENDED=1 ./build.sh
export PATH="$PATH:$(pwd)/target/bin"

# Cross-compile a Mach-O x86_64 binary on Linux:
o64-clang -target x86_64-apple-macos -static -nostdlib -e __start \
          -o bare_metal_test payload.c
```

---

## Architecture

```
iso_find_file() / iso_load_boot_image()
        │
        ▼
┌─────────────────────────────────────────────────┐
│  Pass 1 — Scan Volume Descriptors               │
│  Starting at sector 16, walk each VD until      │
│  type 255 (terminator) or end of image.         │
│  • iso_load_boot_image: locate type-0 BRVD      │
│  • iso_find_file:       locate type-1 PVD       │
└────────────────────┬────────────────────────────┘
                     │
                     ▼
┌─────────────────────────────────────────────────┐
│  Pass 2 — Parse the target descriptor           │
│  • BRVD: read boot_catalog_lba, validate the    │
│    El Torito validation entry (key check)       │
│  • PVD: read root_dir_record embedded in the    │
│    PVD at byte offset 156                       │
└────────────────────┬────────────────────────────┘
                     │
                     ▼
┌─────────────────────────────────────────────────┐
│  Pass 3 — Extract data                          │
│  • iso_load_boot_image: read boot entry,        │
│    malloc, memcpy the boot image                │
│  • iso_find_file: walk directory tree,          │
│    compare component names, descend into        │
│    subdirectories, return pointer to file data  │
└─────────────────────────────────────────────────┘
```

All LBAs, offsets, and lengths are range-checked against `iso_size` before
any pointer is formed.  No pointer arithmetic produces a value outside the
`[iso_data, iso_data + iso_size)` range.

---

## API Reference

### `iso_load_boot_image`

```c
int iso_load_boot_image(const uint8_t *iso_data, size_t iso_size,
                        iso_boot_image_t *out);
```

Locates and copies the El Torito default boot image from an ISO 9660 image
held in memory.

**Parameters:**

| Parameter | Description |
|-----------|-------------|
| `iso_data` | Pointer to the raw ISO image bytes |
| `iso_size` | Byte length of the buffer |
| `out` | Caller-supplied struct; filled on success |

**Return values:**

| Value | Meaning |
|-------|---------|
| `0` | Success; `out->data` points to a `malloc`'d boot image copy |
| `-1` | `iso_data` or `out` is `NULL`, or `iso_size` is too small |
| `-2` | No El Torito Boot Record VD found |
| `-3` | Boot catalog LBA is out of range or validation entry is corrupt |
| `-4` | No bootable entry in the boot catalog |
| `-5` | Boot image extent is out of range |
| `-6` | `malloc` failed |

Call `iso_free_boot_image(out)` to release `out->data` when finished.

**`iso_boot_image_t` fields:**

| Field | Description |
|-------|-------------|
| `data` | `malloc`'d copy of the boot image bytes |
| `size` | Byte length of the boot image |
| `platform_id` | `ISO_ELTORITO_PLATFORM_X86` (0x00) or `ISO_ELTORITO_PLATFORM_EFI` (0xEF) |
| `media_type` | `ISO_ELTORITO_MEDIA_NO_EMUL` (0) for modern ISOs |
| `load_rba` | Source LBA in the ISO image |

---

### `iso_find_file`

```c
const uint8_t *iso_find_file(const uint8_t *iso_data, size_t iso_size,
                              const char *path, size_t *out_size);
```

Traverses the ISO 9660 directory tree and returns a direct pointer to a
file's extent within `iso_data`.

**Parameters:**

| Parameter | Description |
|-----------|-------------|
| `iso_data` | Pointer to the raw ISO image bytes |
| `iso_size` | Byte length of the buffer |
| `path` | Absolute, slash-separated path, e.g. `"/boot/vmlinuz"` |
| `out_size` | If non-`NULL`, receives the file's byte length on success |

**Notes:**

- Path components are matched **case-insensitively** (ISO 9660 stores names in
  upper case; the caller can pass lower-case paths).
- Version suffixes (`;N`) appended by disc mastering tools are stripped before
  comparison.  `"/VMLINUZ"` and `"/VMLINUZ;1"` both match a disc entry named
  `VMLINUZ;1`.
- Returns a **direct pointer into `iso_data`** — no allocation is performed.
  The pointer is valid for as long as `iso_data` remains valid.
- Returns `NULL` on any error (not found, out-of-range extent, invalid input).

---

### `iso_free_boot_image`

```c
void iso_free_boot_image(iso_boot_image_t *img);
```

Releases the memory allocated by `iso_load_boot_image()`.  Clears `img->data`
to `NULL` and `img->size` to `0`.  Safe to call with `img == NULL` or
`img->data == NULL`.

---

## Quick Start

### Build

```bash
make iso_loader    # builds iso_loader_test (no external files needed)
```

Or build manually:

```bash
clang -std=c11 -Wall -Wextra -O2 -I. \
      -o iso_loader_test iso_loader.c iso_loader_test.c
```

### Run the test

```bash
./iso_loader_test
# Expected output:
# ISO loader test: PASS
```

### Use with a real ISO image

```c
#include "iso_loader.h"
#include <stdio.h>
#include <stdlib.h>

static uint8_t *read_file(const char *path, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    *out_size = (size_t)ftell(f);
    rewind(f);
    uint8_t *buf = malloc(*out_size);
    if (buf) fread(buf, 1, *out_size, f);
    fclose(f);
    return buf;
}

int main(int argc, char *argv[]) {
    if (argc < 2) { fputs("usage: demo <image.iso>\n", stderr); return 1; }

    size_t   iso_size;
    uint8_t *iso_data = read_file(argv[1], &iso_size);
    if (!iso_data) { perror(argv[1]); return 1; }

    /* --- Extract the El Torito boot image --- */
    iso_boot_image_t img;
    int rc = iso_load_boot_image(iso_data, iso_size, &img);
    if (rc == 0) {
        printf("Boot image: %zu bytes, platform 0x%02x, LBA %u\n",
               img.size, img.platform_id, img.load_rba);
        iso_free_boot_image(&img);
    } else {
        fprintf(stderr, "iso_load_boot_image: error %d\n", rc);
    }

    /* --- Find the Linux kernel in a typical distro layout --- */
    size_t         vmlinuz_size;
    const uint8_t *vmlinuz = iso_find_file(iso_data, iso_size,
                                            "/boot/vmlinuz", &vmlinuz_size);
    if (vmlinuz) {
        printf("vmlinuz: %zu bytes at offset %zu\n",
               vmlinuz_size, (size_t)(vmlinuz - iso_data));
    }

    free(iso_data);
    return 0;
}
```

---

## PS4 Integration

Once a disc image is available on the PS4 (loaded from a USB drive, a network
socket, or embedded in the payload), the integration sequence is:

### Step 1 — Obtain the ISO image

```c
/* Option A: read from a file descriptor (requires kernel exploit / jailbreak) */
uint8_t *iso_data = ...; /* read via syscall or sceKernelRead */
size_t   iso_size = ...;

/* Option B: embed a small custom ISO as a byte array (xxd -i image.iso > iso_data.h) */
#include "iso_data.h"  /* generated: unsigned char iso_data[]; unsigned int iso_data_len; */
```

### Step 2 — Extract the El Torito boot image

```c
iso_boot_image_t boot_img;
int rc = iso_load_boot_image(iso_data, iso_size, &boot_img);
if (rc != 0) {
    notify("ISO parse failed");
    return;
}
```

### Step 3 — Load and execute via the Mach-O loader (Darwin boot images)

For a Darwin / macOS installer ISO the boot image is itself a Mach-O binary.
Pass it directly to `test_macho_execution`:

```c
#include "macho_loader.h"

int ok = test_macho_execution(boot_img.data, boot_img.size);
iso_free_boot_image(&boot_img);
```

### Step 4 — For Linux ISOs: extract kernel + initrd

```c
size_t         kernel_size, initrd_size;
const uint8_t *kernel = iso_find_file(iso_data, iso_size,
                                       "/boot/vmlinuz", &kernel_size);
const uint8_t *initrd = iso_find_file(iso_data, iso_size,
                                       "/boot/initrd.img", &initrd_size);
```

Then set up the Linux boot protocol (zero-page, command line, initrd address)
and jump to `kernel + 0x200` (the 64-bit entry point after the setup header).

> **Note:** Full Linux boot support requires populating the
> [`struct boot_params`](https://www.kernel.org/doc/html/latest/x86/boot.html)
> zero-page.  This is beyond the scope of the current PoC but is the logical
> next step.

---

## Testing

The self-contained test (`iso_loader_test.c`) constructs a minimal valid ISO
9660 + El Torito disc image entirely in heap memory and exercises:

| Test | Checks |
|------|--------|
| NULL / invalid inputs | All API functions return the correct error or NULL |
| `iso_load_boot_image` | Correct data, size, platform, media type, LBA |
| `iso_find_file` root file | Correct pointer and size |
| Case-insensitive lookup | `/test.txt` finds a disc entry stored as `TEST.TXT` |
| Subdirectory traversal | `/BOOT/VMLINUZ` correctly descends one level |
| Mixed-case subdir | `/boot/vmlinuz` also matches |
| `iso_find_file` without `out_size` | `NULL` `out_size` pointer is handled safely |
| `iso_free_boot_image` edge cases | `NULL` pointer and zeroed struct do not crash |

```bash
make test     # runs both integration_test and iso_loader_test
```

---

## Limitations and Next Steps

| Limitation | Description |
|------------|-------------|
| No Joliet / Rock Ridge | Only ISO 9660 Level 1 names (8.3, upper-case ASCII) are matched |
| No multi-extent files | Files split across multiple extents (rare) are not supported |
| No El Torito section entries | Only the Initial/Default Entry is read; additional boot entries (e.g. EFI fallback) are ignored |
| No Linux boot protocol | Extracting `vmlinuz` is supported; setting up `struct boot_params` and jumping to the 64-bit entry is left to the caller |
| No filesystem write support | The loader is read-only; installing to the PS4's internal storage requires a separate write layer |

Suggested follow-on work:

1. **Linux boot protocol**: Populate `struct boot_params`, set `initrd_addr` / `initrd_size`, and jump to `kernel_base + 0x200`.
2. **El Torito section entries**: Iterate the boot catalog beyond the initial entry to find an EFI boot image when the BIOS entry is absent.
3. **Joliet support**: Parse the Supplementary VD (type 2) with the Joliet escape sequence to support Unicode file names.
4. **USB / network I/O**: Add a streaming reader so that large ISOs can be parsed without loading the entire image into RAM.
