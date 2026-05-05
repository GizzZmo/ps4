/*
 * macho_loader.h - Mach-O 64-bit type definitions and loader API for the
 *                  PS4 Mach-O proof-of-concept.
 *
 * Only the structures and constants required by the loader are defined here;
 * the full <mach-o/loader.h> header is deliberately omitted so the code
 * remains self-contained and portable to the PS4 (FreeBSD) toolchain.
 */

#ifndef MACHO_LOADER_H
#define MACHO_LOADER_H

#include <stddef.h>
#include <stdint.h>

/* -------------------------------------------------------------------------
 * Mach-O magic numbers
 * ---------------------------------------------------------------------- */
#define MH_MAGIC_64   UINT32_C(0xfeedfacf)  /* 64-bit, native byte order  */
#define MH_CIGAM_64   UINT32_C(0xcffaedfe)  /* 64-bit, reversed byte order */

/* -------------------------------------------------------------------------
 * File types (mach_header_64.filetype)
 * ---------------------------------------------------------------------- */
#define MH_EXECUTE    UINT32_C(0x2)

/* -------------------------------------------------------------------------
 * Load-command identifiers
 * ---------------------------------------------------------------------- */
#define LC_SEGMENT_64 UINT32_C(0x19)
#define LC_MAIN       UINT32_C(0x80000028)
#define LC_UNIXTHREAD UINT32_C(0x5)

/* -------------------------------------------------------------------------
 * Mach-O 64-bit header
 * ---------------------------------------------------------------------- */
typedef struct {
    uint32_t magic;        /* MH_MAGIC_64                     */
    uint32_t cputype;      /* CPU_TYPE_X86_64 == 0x01000007   */
    uint32_t cpusubtype;   /* CPU_SUBTYPE_X86_64_ALL == 3      */
    uint32_t filetype;     /* MH_EXECUTE, MH_DYLIB, …         */
    uint32_t ncmds;        /* number of load commands          */
    uint32_t sizeofcmds;   /* byte size of all load commands   */
    uint32_t flags;        /* MH_* flags                       */
    uint32_t reserved;     /* 64-bit padding                   */
} mach_header_64_t;

/* -------------------------------------------------------------------------
 * Generic load-command header (common prefix of every load command)
 * ---------------------------------------------------------------------- */
typedef struct {
    uint32_t cmd;          /* LC_* constant   */
    uint32_t cmdsize;      /* total byte size */
} load_command_t;

/* -------------------------------------------------------------------------
 * LC_SEGMENT_64 load command
 * ---------------------------------------------------------------------- */
#define MACHO_SEGNAME_LEN 16

typedef struct {
    uint32_t cmd;                       /* LC_SEGMENT_64            */
    uint32_t cmdsize;                   /* includes section structs */
    char     segname[MACHO_SEGNAME_LEN];/* segment name             */
    uint64_t vmaddr;                    /* virtual memory address   */
    uint64_t vmsize;                    /* virtual memory size      */
    uint64_t fileoff;                   /* file offset              */
    uint64_t filesize;                  /* bytes in file            */
    uint32_t maxprot;                   /* maximum VM protection    */
    uint32_t initprot;                  /* initial VM protection    */
    uint32_t nsects;                    /* number of sections       */
    uint32_t flags;                     /* segment flags            */
} segment_command_64_t;

/* -------------------------------------------------------------------------
 * LC_MAIN load command (entry_point_command)
 * ---------------------------------------------------------------------- */
typedef struct {
    uint32_t cmd;          /* LC_MAIN                             */
    uint32_t cmdsize;      /* 24                                  */
    uint64_t entryoff;     /* file (__TEXT) offset of entry point */
    uint64_t stacksize;    /* initial stack size (0 == default)   */
} entry_point_command_t;

/* -------------------------------------------------------------------------
 * VM-protection bit masks  (mirrors <sys/mman.h> on FreeBSD / PS4)
 * ---------------------------------------------------------------------- */
#define VM_PROT_NONE    0x00
#define VM_PROT_READ    0x01
#define VM_PROT_WRITE   0x02
#define VM_PROT_EXEC    0x04

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

/*
 * load_mach_o_segments() - Parse a 64-bit Mach-O image, map each
 * LC_SEGMENT_64 into anonymous memory, and return the entry-point address.
 *
 * Parameters:
 *   macho_data   - pointer to the raw Mach-O bytes in memory
 *   size         - byte length of the buffer
 *   out_map_base - optional; if non-NULL, receives the base address of the
 *                  mmap reservation (needed to call munmap later).
 *   out_map_size - optional; if non-NULL, receives the total byte size of
 *                  the mmap reservation.
 *
 * Returns:
 *   A non-NULL pointer to the entry point on success; NULL on any error
 *   (bad magic, missing entry point, allocation failure, …).
 *   out_map_base and out_map_size are only written on success.
 *
 * Notes:
 *   * Mapped memory is initially RW; the caller must call mprotect (or the
 *     PS4 equivalent sceKernelMprotect) to add VM_PROT_EXEC before jumping
 *     to the returned address.
 *   * The caller is responsible for releasing the mapping with munmap() when
 *     it is no longer needed.  Use out_map_base and out_map_size to obtain
 *     the correct base and size for the munmap call.
 *   * Only LC_SEGMENT_64 and LC_MAIN / LC_UNIXTHREAD are examined; all
 *     other load commands are skipped.
 */
void *load_mach_o_segments(const uint8_t *macho_data, size_t size,
                            void **out_map_base, size_t *out_map_size);

/*
 * test_macho_execution() - Map, execute, and validate a bare-metal Mach-O
 * payload.
 *
 * The function:
 *   1. Calls load_mach_o_segments() to map the image.
 *   2. Applies mprotect(RX) so the code can be executed (DEP / NX bypass).
 *   3. Issues an mfence to serialise any preceding stores.
 *   4. Casts the entry point to a function pointer and calls it.
 *   5. Validates that the return value equals 0x1379.
 *
 * Returns 1 on success (return value == 0x1379), 0 on any failure.
 */
int test_macho_execution(const uint8_t *macho_data, size_t size);

#endif /* MACHO_LOADER_H */
