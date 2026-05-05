# API Reference — PS4 Mach-O Loader

Complete reference for all public types, constants, and functions defined in `macho_loader.h` and implemented in `macho_loader.c` / `macho_test.c`.

---

## Table of Contents

1. [Header: `macho_loader.h`](#header-macho_loaderh)
   - [Magic Numbers](#magic-numbers)
   - [File Types](#file-types)
   - [Load-Command Identifiers](#load-command-identifiers)
   - [Struct: `mach_header_64_t`](#struct-mach_header_64_t)
   - [Struct: `load_command_t`](#struct-load_command_t)
   - [Struct: `segment_command_64_t`](#struct-segment_command_64_t)
   - [Struct: `entry_point_command_t`](#struct-entry_point_command_t)
   - [VM Protection Flags](#vm-protection-flags)
2. [Function: `load_mach_o_segments`](#function-load_mach_o_segments)
3. [Function: `test_macho_execution`](#function-test_macho_execution)

---

## Header: `macho_loader.h`

Include this header in any translation unit that calls the loader API:

```c
#include "macho_loader.h"
```

No other headers are required; `macho_loader.h` pulls in only `<stddef.h>` and `<stdint.h>`.

---

### Magic Numbers

```c
#define MH_MAGIC_64   UINT32_C(0xfeedfacf)
#define MH_CIGAM_64   UINT32_C(0xcffaedfe)
```

| Constant | Value | Meaning |
|----------|-------|---------|
| `MH_MAGIC_64` | `0xfeedfacf` | 64-bit Mach-O, native byte order (little-endian on x86-64) |
| `MH_CIGAM_64` | `0xcffaedfe` | 64-bit Mach-O, byte-swapped (big-endian) — **not supported** by this loader |

The loader validates `hdr->magic == MH_MAGIC_64` and returns `NULL` for any other value.

---

### File Types

```c
#define MH_EXECUTE    UINT32_C(0x2)
```

| Constant | Value | Meaning |
|----------|-------|---------|
| `MH_EXECUTE` | `0x2` | Stand-alone executable |

Only `MH_EXECUTE` is relevant for this PoC. The file-type field is not validated by the loader; it is provided for informational use.

---

### Load-Command Identifiers

```c
#define LC_SEGMENT_64 UINT32_C(0x19)
#define LC_MAIN       UINT32_C(0x80000028)
#define LC_UNIXTHREAD UINT32_C(0x5)
```

| Constant | Value | Meaning |
|----------|-------|---------|
| `LC_SEGMENT_64` | `0x19` | 64-bit segment mapping command |
| `LC_MAIN` | `0x80000028` | Modern entry-point command (OS X 10.8+) |
| `LC_UNIXTHREAD` | `0x05` | Legacy entry-point via full thread state |

---

### Struct: `mach_header_64_t`

```c
typedef struct {
    uint32_t magic;        /* MH_MAGIC_64                     */
    uint32_t cputype;      /* CPU_TYPE_X86_64 == 0x01000007   */
    uint32_t cpusubtype;   /* CPU_SUBTYPE_X86_64_ALL == 3     */
    uint32_t filetype;     /* MH_EXECUTE, MH_DYLIB, …        */
    uint32_t ncmds;        /* number of load commands         */
    uint32_t sizeofcmds;   /* byte size of all load commands  */
    uint32_t flags;        /* MH_* flags                      */
    uint32_t reserved;     /* 64-bit padding                  */
} mach_header_64_t;
```

Located at offset `0` of any 64-bit Mach-O file.

---

### Struct: `load_command_t`

```c
typedef struct {
    uint32_t cmd;          /* LC_* constant   */
    uint32_t cmdsize;      /* total byte size */
} load_command_t;
```

Common prefix shared by every load command. The loader casts `cmd_ptr` to `load_command_t *` to read the command type and advance the pointer by `cmdsize`.

---

### Struct: `segment_command_64_t`

```c
#define MACHO_SEGNAME_LEN 16

typedef struct {
    uint32_t cmd;                        /* LC_SEGMENT_64            */
    uint32_t cmdsize;                    /* includes section structs */
    char     segname[MACHO_SEGNAME_LEN]; /* segment name             */
    uint64_t vmaddr;                     /* virtual memory address   */
    uint64_t vmsize;                     /* virtual memory size      */
    uint64_t fileoff;                    /* file offset              */
    uint64_t filesize;                   /* bytes in file            */
    uint32_t maxprot;                    /* maximum VM protection    */
    uint32_t initprot;                   /* initial VM protection    */
    uint32_t nsects;                     /* number of sections       */
    uint32_t flags;                      /* segment flags            */
} segment_command_64_t;
```

Section structs (if any) immediately follow this struct inside the load-command region; their total size is included in `cmdsize`.

---

### Struct: `entry_point_command_t`

```c
typedef struct {
    uint32_t cmd;          /* LC_MAIN                             */
    uint32_t cmdsize;      /* 24                                  */
    uint64_t entryoff;     /* file (__TEXT) offset of entry point */
    uint64_t stacksize;    /* initial stack size (0 == default)   */
} entry_point_command_t;
```

`entryoff` is a byte offset measured from the start of the `__TEXT` segment mapping (i.e. from `vm_min` / `base`).

---

### VM Protection Flags

```c
#define VM_PROT_NONE    0x00
#define VM_PROT_READ    0x01
#define VM_PROT_WRITE   0x02
#define VM_PROT_EXEC    0x04
```

Mirror the `<sys/mman.h>` constants on FreeBSD / PS4. Combine with bitwise OR:

| Combination | Meaning |
|-------------|---------|
| `VM_PROT_READ \| VM_PROT_WRITE` | Read-Write (initial state after `mmap`) |
| `VM_PROT_READ \| VM_PROT_EXEC` | Read-Execute (required before calling entry point) |

---

## Function: `load_mach_o_segments`

**Declared in:** `macho_loader.h`  
**Implemented in:** `macho_loader.c`

```c
void *load_mach_o_segments(const uint8_t *macho_data, size_t size,
                            void **out_map_base, size_t *out_map_size);
```

### Description

Parses a 64-bit Mach-O image held entirely in memory, maps each `LC_SEGMENT_64` into a new anonymous memory region, and returns the address of the binary's entry point.

The function performs three sequential passes:

1. **Measure** — walk all `LC_SEGMENT_64` commands to compute the total virtual-address span.
2. **Allocate** — make a single `mmap(PROT_READ|PROT_WRITE)` reservation.
3. **Copy** — `memcpy` each segment's raw bytes from `macho_data` into the mapped region.
4. **Locate** — find `LC_MAIN` or `LC_UNIXTHREAD` and return the entry-point address.

### Parameters

| Parameter | Type | Description |
|-----------|------|-------------|
| `macho_data` | `const uint8_t *` | Pointer to the start of the Mach-O image in memory. Must not be `NULL`. |
| `size` | `size_t` | Byte length of the buffer pointed to by `macho_data`. |
| `out_map_base` | `void **` | Optional (may be `NULL`). On success, receives the base address of the `mmap` reservation. Pass the value to `munmap` when finished. |
| `out_map_size` | `size_t *` | Optional (may be `NULL`). On success, receives the total byte size of the `mmap` reservation. Pass the value to `munmap` and `mprotect`. |

`out_map_base` and `out_map_size` are **only written on success** (non-`NULL` return value).

### Return Value

| Value | Meaning |
|-------|---------|
| Non-`NULL` pointer | Address of the entry point within the newly mapped region. |
| `NULL` | One of the error conditions listed below. |

### Error Conditions (returns `NULL`)

| Condition | Cause |
|-----------|-------|
| `macho_data == NULL` | Null input pointer |
| `size < sizeof(mach_header_64_t)` | Buffer too small for even the header |
| `hdr->magic != MH_MAGIC_64` | Not a 64-bit Mach-O (wrong magic or byte-swapped) |
| Header + load-command region exceeds `size` | Malformed `sizeofcmds` |
| Malformed `cmdsize` (< `sizeof(load_command_t)` or overflows region) | Corrupted load commands |
| No `LC_SEGMENT_64` with non-zero `vmsize` found | Nothing to map |
| `mmap()` fails | Out of memory or address space |
| Segment `fileoff+filesize` exceeds `size` | Segment data out of bounds in input |
| Segment destination out of reserved region | Malformed `vmaddr` or `vmsize` |
| No `LC_MAIN` or `LC_UNIXTHREAD` found | No entry point declared |
| `LC_MAIN` `cmdsize` too small | Malformed entry-point command |
| `LC_MAIN` `entryoff` outside the mapped region | Entry point out of bounds |
| `LC_UNIXTHREAD` computed RIP outside the mapped region | Entry point out of bounds |

### Caller Responsibilities

1. **Memory protection**: the returned pointer points into a `PROT_READ|PROT_WRITE` mapping. You **must** call `mprotect` (or `sceKernelMprotect` on PS4) to add `PROT_EXEC` before executing the returned pointer.  Use `out_map_base` and `out_map_size` as the address and length arguments to `mprotect` so that the entire reservation is covered.
2. **Memory lifetime**: the mapping is *not* freed by this function. Call `munmap(out_map_base, out_map_size)` when the mapping is no longer needed to avoid a memory leak.

### Example

```c
#include "macho_loader.h"
#include <sys/mman.h>

void run_payload(const uint8_t *data, size_t len) {
    void  *map_base = NULL;
    size_t map_size = 0;

    void *entry = load_mach_o_segments(data, len, &map_base, &map_size);
    if (!entry) {
        /* parse/map failed */
        return;
    }

    if (mprotect(map_base, map_size, PROT_READ | PROT_EXEC) != 0) {
        munmap(map_base, map_size);
        return;
    }

    typedef int (*fn_t)(void);
    int result = ((fn_t)(uintptr_t)entry)();
    /* use result */

    munmap(map_base, map_size);
}
```

---

## Function: `test_macho_execution`

**Declared in:** `macho_loader.h`  
**Implemented in:** `macho_test.c`

```c
int test_macho_execution(const uint8_t *macho_data, size_t size);
```

### Description

Convenience wrapper that orchestrates the full load-and-execute pipeline for the bare-metal PoC payload:

1. Calls `load_mach_o_segments()`.
2. Promotes mapped pages from `RW` to `RX` using `ps4_mprotect`.
3. Issues `__builtin_ia32_mfence()` as a store-serialisation barrier.
4. Casts the entry-point address to `int (*)(void)` and calls it.
5. Returns `1` if the result equals `0x1379`, `0` otherwise.

### Parameters

Same as `load_mach_o_segments`.

### Return Value

| Value | Meaning |
|-------|---------|
| `1` | Success — payload executed and returned `0x1379` |
| `0` | Failure — any error in steps 1–5 |

### `ps4_mprotect` Macro

```c
// When compiled with -DSCE_KERNEL_MPROTECT_AVAILABLE:
#define ps4_mprotect(addr, len, prot)  sceKernelMprotect((addr), (len), (prot))

// Otherwise (development host):
#define ps4_mprotect(addr, len, prot)  mprotect((addr), (len), (prot))
```

Define `SCE_KERNEL_MPROTECT_AVAILABLE` when compiling for the PS4 SDK to route the call through `sceKernelMprotect`.
