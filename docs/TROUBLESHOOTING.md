# Troubleshooting — PS4 Mach-O Loader

This guide covers the most common failure modes, their root causes, and step-by-step diagnostic procedures.

---

## Table of Contents

1. [Quick Diagnostic Checklist](#quick-diagnostic-checklist)
2. [Loader Returns `NULL`](#loader-returns-null)
3. [Segmentation Fault on Execution](#segmentation-fault-on-execution)
4. [Wrong Return Value (not `0x1379`)](#wrong-return-value-not-0x1379)
5. [`mprotect` / `sceKernelMprotect` Fails](#mprotect--scekernelmprotect-fails)
6. [Cross-Compilation Fails on Linux](#cross-compilation-fails-on-linux)
7. [Loader Compiles but Crashes at Runtime on PS4](#loader-compiles-but-crashes-at-runtime-on-ps4)
8. [Symbol Not Found: `__start`](#symbol-not-found-__start)
9. [Gathering Diagnostic Information](#gathering-diagnostic-information)

---

## Quick Diagnostic Checklist

Work through this list in order before diving into the sections below:

- [ ] `file bare_metal_test` reports `Mach-O 64-bit executable x86_64`
- [ ] `otool -l bare_metal_test` (or `objdump -p`) shows `LC_SEGMENT_64` with non-zero `vmsize`
- [ ] `otool -l bare_metal_test` shows `LC_MAIN` with a non-zero `entryoff` **or** `LC_UNIXTHREAD`
- [ ] `nm bare_metal_test` lists `__start` (two underscores)
- [ ] On the development host, the loader test binary runs without a crash
- [ ] On PS4, the kernel exploit is confirmed active before calling the loader

---

## Loader Returns `NULL`

`load_mach_o_segments` returns `NULL` for any of the following reasons.

### Bad magic number

**Symptom:** The first field of the buffer is not `0xfeedfacf`.

**Diagnosis:**

```bash
xxd -l 4 bare_metal_test
# Expected: cf fa ed fe  (little-endian 0xfeedfacf)
```

**Fixes:**
- Confirm the file is a 64-bit Mach-O: `file bare_metal_test`
- If it shows `Mach-O 64-bit executable arm64`, you compiled for the wrong architecture. Re-run with `-target x86_64-apple-macos`.
- If it shows `ELF`, the cross-compilation step was omitted. See [Build Guide](BUILD.md).

---

### Buffer too small

**Symptom:** `size < sizeof(mach_header_64_t)` (32 bytes).

**Diagnosis:**

```bash
wc -c bare_metal_test
# Must be > 32 bytes; a realistic minimum is ~4 KB
```

---

### No loadable segments

**Symptom:** All `LC_SEGMENT_64` entries have `vmsize == 0`, or there are none.

**Diagnosis:**

```bash
otool -l bare_metal_test | grep -A6 LC_SEGMENT_64
# Look for vmsize > 0
```

---

### Malformed load commands

**Symptom:** A `cmdsize` of zero or a value that would walk past `sizeofcmds`.

**Diagnosis:**

```bash
otool -l bare_metal_test
# Any command with cmdsize 0 is corrupted
```

Re-compile `payload.c` from scratch. If the file was transferred over a network, verify the checksum:

```bash
md5sum bare_metal_test   # on build machine
md5sum bare_metal_test   # on target / after embedding
```

---

### `mmap` failure

**Symptom:** `load_mach_o_segments` returns `NULL` even with a valid Mach-O.

**Diagnosis (development host):**

```c
#include <errno.h>
#include <string.h>
// Temporarily add this after the mmap call in macho_loader.c:
fprintf(stderr, "mmap error: %s\n", strerror(errno));
```

**Common causes:**
- `total_size` overflows `size_t` due to a malformed `vmsize`.
- The kernel's virtual address space is exhausted (unlikely on 64-bit but possible in tests).
- On PS4: the kernel exploit has not unlocked `mmap` for user-land use.

---

### No entry-point command

**Symptom:** Neither `LC_MAIN` nor `LC_UNIXTHREAD` found.

**Diagnosis:**

```bash
otool -l bare_metal_test | grep -E "LC_MAIN|LC_UNIXTHREAD"
```

If neither appears, the binary was linked without an entry point. Re-compile with `-e __start`.

---

## Segmentation Fault on Execution

A segfault when calling the entry-point pointer almost always means the pages are not executable.

### `mprotect` was not called

Verify that `mprotect` (or `sceKernelMprotect`) was called **and succeeded** before the indirect call:

```c
int rc = ps4_mprotect(page_base, prot_size, VM_PROT_READ | VM_PROT_EXEC);
if (rc != 0) {
    /* handle error – do NOT proceed to call the entry point */
}
```

### `mprotect` was called but on the wrong address range

The `page_base` computation in `macho_test.c` aligns down to the nearest 4 KiB page boundary. Ensure you are passing the correct base and an appropriate size:

```c
void  *page_base = (void *)((uintptr_t)entry_addr & ~(uintptr_t)0xfff);
size_t prot_size = (size + 0xfff) & ~(size_t)0xfff;  /* page-align up */
```

### Kernel exploit did not unlock execution

Some PS4 kernel exploits require an explicit step to enable user-land code execution (e.g. clearing a kernel flag). Consult your exploit's documentation.

---

## Wrong Return Value (not `0x1379`)

`test_macho_execution` returns `0` if `execute_payload()` does not return exactly `0x1379` (`= 0x1337 + 0x42`).

### Entry point resolves to the wrong function

```bash
# Check the entry offset
otool -l bare_metal_test | grep entryoff
# Check the disassembly at that offset
otool -tv bare_metal_test | head -40
# The first instruction should be the _start function body
```

If the entry offset points into a stub or CRT initialiser instead of `_start`, the `-nostdlib -e __start` flags may not have been passed correctly.

### Code was not copied correctly

If the segment was not `memcpy`'d properly (e.g. wrong `fileoff` or `filesize`), the mapped pages will contain zeros and the "function" will execute NOP sleds or invalid instructions.

Add a temporary check:

```c
// After load_mach_o_segments returns, inspect the first bytes at entry_addr
uint8_t *p = (uint8_t *)entry_addr;
for (int i = 0; i < 16; i++) fprintf(stderr, "%02x ", p[i]);
fprintf(stderr, "\n");
```

Compare against the disassembly of `_start` from `otool -tv bare_metal_test`.

---

## `mprotect` / `sceKernelMprotect` Fails

### Development host

`mprotect` can fail if:
- The address range spans multiple mappings with incompatible permissions.
- The `prot_size` is so large it extends past the end of the mapping.

Use `page_align(size)` rather than a fixed size to stay within the mapped region.

### PS4

`sceKernelMprotect` can fail if:
- The kernel exploit is not active.
- The target memory was not allocated with `mmap` (e.g. it is stack memory).
- The `VM_PROT_EXEC` flag is blocked by a kernel security policy that the exploit has not yet disabled.

Check the return code and errno:

```c
int rc = sceKernelMprotect(page_base, prot_size,
                            VM_PROT_READ | VM_PROT_EXEC);
if (rc != 0) {
    char msg[64];
    snprintf(msg, sizeof(msg), "mprotect failed: %d", rc);
    notify(msg);
}
```

---

## Cross-Compilation Fails on Linux

### Error: cannot find `x86_64-apple-macos` target

Clang on Linux does not ship with the macOS sysroot by default. Use [osxcross](https://github.com/tpoechtrager/osxcross):

```bash
# After setting up osxcross:
export PATH="$PATH:/path/to/osxcross/target/bin"
o64-clang -target x86_64-apple-macos -static -nostdlib -e __start \
          -o bare_metal_test payload.c
```

### Error: `ld: unknown option: -new_linker`

Remove `-Wl,-new_linker` from the command line. This flag is only supported on recent Apple linkers.

### Error: `ld: can't open output file`

Check that the output directory exists and is writable:

```bash
ls -la .
touch bare_metal_test && rm bare_metal_test
```

---

## Loader Compiles but Crashes at Runtime on PS4

### Undefined symbol: `mprotect`

The PS4 SDK does not export the POSIX `mprotect` symbol. Recompile with:

```bash
-DSCE_KERNEL_MPROTECT_AVAILABLE
```

This switches `ps4_mprotect` to `sceKernelMprotect`.

### Stack corruption / immediate crash

Ensure the payload is a valid **static, nostdlib** binary. Any CRT startup code that assumes a Darwin environment (e.g. calls `dyld_stub_binder`) will crash immediately on FreeBSD.

Confirm with:

```bash
otool -l bare_metal_test | grep LC_LOAD_DYLIB
# Should produce no output for a -nostdlib binary
```

---

## Symbol Not Found: `__start`

**Symptom:** The linker reports `symbol(s) not found for architecture x86_64` with a reference to `__start`.

**Cause:** The C source uses `_start` (one underscore) as the function name. Darwin tools prepend an extra underscore, making it `__start` in the symbol table. Passing `-e __start` (two underscores) to the linker is correct and expected.

**Verification:**

```bash
nm bare_metal_test | grep start
# Should show: 0000000100003f50 T __start
```

If the symbol shows as `_start` (one underscore), you are using a non-Darwin (Linux ELF) linker. Switch to the Clang cross-compiler with `-target x86_64-apple-macos`.

---

## Gathering Diagnostic Information

When filing a bug report, include the following:

```bash
# 1. File type
file bare_metal_test

# 2. Mach-O load commands
otool -l bare_metal_test

# 3. Symbol table
nm -a bare_metal_test

# 4. Disassembly of __TEXT
otool -tv bare_metal_test

# 5. Hex dump of first 64 bytes
xxd -l 64 bare_metal_test

# 6. Compiler version
clang --version

# 7. OS and kernel version
uname -a
```
