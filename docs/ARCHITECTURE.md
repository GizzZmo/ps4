# Architecture — PS4 Mach-O Loader

This document explains the internal design of the loader in depth: the Mach-O binary format, how the three-pass algorithm works, ASLR handling, and the memory model used throughout.

---

## Table of Contents

1. [Mach-O Format Primer](#mach-o-format-primer)
2. [Loader Design](#loader-design)
   - [Pass 1 — Measure](#pass-1--measure)
   - [Single mmap Reservation](#single-mmap-reservation)
   - [Pass 2 — Copy Segments](#pass-2--copy-segments)
   - [Pass 3 — Locate Entry Point](#pass-3--locate-entry-point)
3. [ASLR and the Slide](#aslr-and-the-slide)
4. [Memory Model](#memory-model)
5. [Supported Load Commands](#supported-load-commands)
6. [Execution Handoff](#execution-handoff)
7. [Design Decisions and Trade-offs](#design-decisions-and-trade-offs)

---

## Mach-O Format Primer

A 64-bit Mach-O file has three logical regions:

```
┌─────────────────────────┐  ← offset 0
│  mach_header_64         │  32 bytes: magic, cputype, ncmds, sizeofcmds, …
├─────────────────────────┤
│  Load Commands          │  sizeofcmds bytes, ncmds entries
│    LC_SEGMENT_64 [0]    │
│    LC_SEGMENT_64 [1]    │
│    LC_MAIN              │  (or LC_UNIXTHREAD for older toolchains)
│    …                    │
├─────────────────────────┤
│  Raw Data               │  Segment contents, symbol tables, etc.
└─────────────────────────┘
```

### `mach_header_64`

| Field | Size | Notes |
|-------|------|-------|
| `magic` | 4 B | `0xfeedfacf` for 64-bit LE; `0xcffaedfe` for 64-bit BE |
| `cputype` | 4 B | `0x01000007` = `CPU_TYPE_X86_64` |
| `cpusubtype` | 4 B | `3` = `CPU_SUBTYPE_X86_64_ALL` |
| `filetype` | 4 B | `2` = `MH_EXECUTE` |
| `ncmds` | 4 B | Number of load commands |
| `sizeofcmds` | 4 B | Total byte length of all load commands |
| `flags` | 4 B | Binary behaviour flags |
| `reserved` | 4 B | Padding for 8-byte alignment |

### `LC_SEGMENT_64`

Each `LC_SEGMENT_64` describes one segment (e.g. `__TEXT`, `__DATA`):

| Field | Notes |
|-------|-------|
| `segname` | 16-char zero-padded ASCII name |
| `vmaddr` | Preferred virtual address in the process address space |
| `vmsize` | Size of the virtual address range (may exceed `filesize`) |
| `fileoff` | Byte offset of segment content in the Mach-O file |
| `filesize` | Bytes of content in the file (≤ `vmsize`); zero-fill the rest |
| `maxprot` / `initprot` | Maximum and initial VM protection (RWX bits) |
| `nsects` | Number of sections inside this segment |

### `LC_MAIN`

Modern Apple toolchains emit `LC_MAIN` (introduced in OS X 10.8):

| Field | Notes |
|-------|-------|
| `entryoff` | File offset of the entry point from the start of the `__TEXT` segment (i.e. from `vm_min` in this loader) |
| `stacksize` | Requested initial stack size; `0` = use the system default |

### `LC_UNIXTHREAD`

Legacy entry-point command used by older toolchains:

- Contains a full CPU register state.
- For x86-64 the `rip` field (at byte offset 144 from the start of the command) holds the virtual address of the entry point.
- The loader applies the ASLR slide to this address.

---

## Loader Design

### Pass 1 — Measure

```c
uint64_t vm_min = UINT64_MAX;
uint64_t vm_max = 0;

for each LC_SEGMENT_64:
    vm_min = min(vm_min, seg->vmaddr)
    vm_max = max(vm_max, seg->vmaddr + seg->vmsize)
```

Goal: compute the tightest bounding box around all loadable segments so that a **single** `mmap()` call covers the entire virtual-address span.

Safety checks performed per load command:
- `cmdsize ≥ sizeof(load_command_t)` — prevents zero-size or truncated commands.
- Accumulated offset stays within `sizeof(mach_header_64_t) + sizeofcmds` — prevents walking off the declared command region.

### Single mmap Reservation

```
total_size = vm_max - vm_min
```

A single anonymous, private, RW mapping is made:

```c
// Preferred: map at the binary's own vmaddr
mmap((void*)vm_min, total_size, PROT_READ|PROT_WRITE,
     MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE, -1, 0)

// Fallback: let the kernel choose (triggers ASLR slide)
mmap(NULL, total_size, PROT_READ|PROT_WRITE,
     MAP_PRIVATE|MAP_ANONYMOUS, -1, 0)
```

`MAP_FIXED_NOREPLACE` (Linux ≥ 4.17) is used when available so the loader does not silently overwrite existing mappings. If it fails (address already in use), the kernel picks a free address and the slide calculation compensates.

The entire reservation is zeroed with `memset` to implement the BSS zero-fill requirement.

### Pass 2 — Copy Segments

```c
for each LC_SEGMENT_64:
    dest = base + (seg->vmaddr - vm_min)
    src  = macho_data + seg->fileoff
    memcpy(dest, src, seg->filesize)
```

Safety checks:
- `seg->fileoff + seg->filesize ≤ size` — source does not exceed input buffer.
- `dest + seg->filesize ≤ base + total_size` — destination fits in the reservation.

Any `vmsize > filesize` bytes are already zero (from `memset` above), providing the BSS zero-fill.

### Pass 3 — Locate Entry Point

```
LC_MAIN:
    entry_addr = base + ep->entryoff

LC_UNIXTHREAD (x86_64):
    rip_offset = 16 (fixed header) + 16 * 8 (16 preceding registers)
               = 144 bytes from command start
    rip        = *(uint64_t*)(cmd_ptr + rip_offset)
    entry_addr = (void*)(rip + slide)
```

`LC_MAIN` is preferred over `LC_UNIXTHREAD`; the loop breaks as soon as either is found.

---

## ASLR and the Slide

```
slide = (uintptr_t)base - vm_min
```

When `mmap()` returns an address different from the requested `vm_min`, all absolute virtual addresses embedded in the binary must be rebased by this delta.

- `LC_MAIN` stores `entryoff` as a **relative** offset from `vm_min`, so `base + entryoff` is already correct regardless of the slide.
- `LC_UNIXTHREAD` stores the entry point as an **absolute** virtual address (`rip`), so the slide must be added: `rip + slide`.

---

## Memory Model

```
After load_mach_o_segments() returns:

base ──► ┌────────────────────┐ PROT_READ | PROT_WRITE
         │  __TEXT segment    │
         │  (code, read-only  │
         │   data)            │
         ├────────────────────┤
         │  __DATA segment    │
         │  (globals, BSS)    │
         └────────────────────┘
         base + total_size

The caller (macho_test.c) then calls:

mprotect(page_base, prot_size, PROT_READ | PROT_EXEC)

After which the memory is:

base ──► ┌────────────────────┐ PROT_READ | PROT_EXEC
         │  (now executable)  │
         …
```

> **Important:** Never map memory as `PROT_WRITE | PROT_EXEC` simultaneously on production systems. Always separate the write phase (Pass 2) from the execute phase (after mprotect).

---

## Supported Load Commands

| Command | Value | Handled |
|---------|-------|---------|
| `LC_SEGMENT_64` | `0x19` | ✅ Mapped in passes 1 and 2 |
| `LC_MAIN` | `0x80000028` | ✅ Entry point in pass 3 |
| `LC_UNIXTHREAD` | `0x05` | ✅ Entry point (x86-64 RIP) in pass 3 |
| All others | — | ⏭ Silently skipped |

Commands such as `LC_DYLD_INFO`, `LC_SYMTAB`, and `LC_LOAD_DYLIB` are intentionally ignored. The loader only handles **static, nostdlib** Mach-O binaries.

---

## Execution Handoff

After `mprotect` and `mfence`, the entry point is called via a typed function pointer:

```c
typedef int (*payload_entry_t)(void);
payload_entry_t fn = (payload_entry_t)(uintptr_t)entry_addr;
int result = fn();
```

The double cast through `uintptr_t` is the accepted portable workaround for converting between data and function pointers in C (the C standard does not allow a direct cast).

The `__builtin_ia32_mfence()` intrinsic ensures that:
1. All `memcpy` stores (inside `load_mach_o_segments`) are visible to the I-cache before the indirect call.
2. The `mprotect` permission change is fully committed before execution begins.

---

## Design Decisions and Trade-offs

| Decision | Rationale |
|----------|-----------|
| Single contiguous `mmap` reservation | Mirrors how the macOS kernel and most ELF loaders work; avoids per-segment mapping bookkeeping |
| `MAP_FIXED_NOREPLACE` with fallback | Prefers the binary's own preferred address for compatibility, but never stomps existing mappings |
| Three passes instead of one | Separating measurement, allocation, copying, and entry-point detection makes each step independently verifiable and auditable |
| No dynamic linking support | Out of scope for this PoC; a full `dyld` emulation layer would be needed for production use |
| No Darwin syscall translation | Payloads that call macOS-specific syscalls will crash on PS4; a syscall table shim is the logical next step |
