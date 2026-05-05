/*
 * bare_metal_test_data.h - Minimal x86-64 Mach-O payload embedded as a C
 *                          byte array for self-contained integration testing.
 *
 * The Mach-O image was constructed by hand to avoid a cross-compilation
 * toolchain dependency in CI.  It is functionally equivalent to the output
 * produced by compiling payload.c with:
 *
 *   clang -target x86_64-apple-macos -static -nostdlib -e __start \
 *         -o bare_metal_test payload.c
 *
 * Image layout (134 bytes total):
 *   [0x00]  mach_header_64          (32 bytes)
 *   [0x20]  LC_SEGMENT_64 __TEXT    (72 bytes, 0 sections, fileoff=0)
 *   [0x68]  LC_MAIN                 (24 bytes, entryoff=0x80)
 *   [0x80]  Entry code              (6 bytes)
 *             b8 79 13 00 00   mov eax, 0x1379   ; 0x1337 + 0x42
 *             c3               ret
 *
 * Expected behaviour: the entry function returns 0x1379.
 */

#ifndef BARE_METAL_TEST_DATA_H
#define BARE_METAL_TEST_DATA_H

#include <stddef.h>
#include <stdint.h>

/* clang-format off */
static const uint8_t bare_metal_test_data[] = {
    /* mach_header_64 (32 bytes) */
    0xcf, 0xfa, 0xed, 0xfe,  /* magic     = MH_MAGIC_64  (0xfeedfacf LE) */
    0x07, 0x00, 0x00, 0x01,  /* cputype   = CPU_TYPE_X86_64              */
    0x03, 0x00, 0x00, 0x00,  /* cpusubtype= CPU_SUBTYPE_X86_64_ALL       */
    0x02, 0x00, 0x00, 0x00,  /* filetype  = MH_EXECUTE                   */
    0x02, 0x00, 0x00, 0x00,  /* ncmds     = 2                            */
    0x60, 0x00, 0x00, 0x00,  /* sizeofcmds= 96  (72 + 24)               */
    0x00, 0x00, 0x00, 0x00,  /* flags     = 0                            */
    0x00, 0x00, 0x00, 0x00,  /* reserved  = 0                            */

    /* LC_SEGMENT_64 __TEXT (72 bytes, no sections) */
    0x19, 0x00, 0x00, 0x00,  /* cmd       = LC_SEGMENT_64 (0x19)         */
    0x48, 0x00, 0x00, 0x00,  /* cmdsize   = 72                           */
    0x5f, 0x5f, 0x54, 0x45,
    0x58, 0x54, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,  /* segname   = "__TEXT"                     */
    0x00, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00,  /* vmaddr    = 0x100000000                  */
    0x00, 0x10, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,  /* vmsize    = 0x1000                       */
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,  /* fileoff   = 0                            */
    0x86, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,  /* filesize  = 134                          */
    0x07, 0x00, 0x00, 0x00,  /* maxprot   = VM_PROT_READ|WRITE|EXEC      */
    0x05, 0x00, 0x00, 0x00,  /* initprot  = VM_PROT_READ|EXEC            */
    0x00, 0x00, 0x00, 0x00,  /* nsects    = 0                            */
    0x00, 0x00, 0x00, 0x00,  /* flags     = 0                            */

    /* LC_MAIN (24 bytes) */
    0x28, 0x00, 0x00, 0x80,  /* cmd       = LC_MAIN (0x80000028)         */
    0x18, 0x00, 0x00, 0x00,  /* cmdsize   = 24                           */
    0x80, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,  /* entryoff  = 0x80 (128) from base         */
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,  /* stacksize = 0 (use default)              */

    /* Entry-point code at offset 0x80 */
    0xb8, 0x79, 0x13, 0x00, 0x00,  /* mov eax, 0x1379  (0x1337 + 0x42) */
    0xc3                            /* ret                               */
};
/* clang-format on */

static const size_t bare_metal_test_size =
    sizeof(bare_metal_test_data);

#endif /* BARE_METAL_TEST_DATA_H */
