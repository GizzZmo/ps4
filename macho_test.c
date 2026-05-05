/*
 * macho_test.c - Execution harness for the PS4 Mach-O loader PoC.
 *
 * test_macho_execution() takes a raw Mach-O image in memory, maps it via
 * load_mach_o_segments(), upgrades the mapped pages to RX (bypassing DEP /
 * NX), and then calls the entry point.  The return value is validated
 * against 0x1379 – the expected result produced by the bare-metal payload
 * in payload.c (0x1337 + 0x42).
 */

#include "macho_loader.h"

#include <stddef.h>
#include <stdint.h>
#include <sys/mman.h>   /* mprotect */

/* -------------------------------------------------------------------------
 * PS4 / FreeBSD memory-protection helper
 *
 * On a retail PS4 the kernel exposes sceKernelMprotect() rather than the
 * standard mprotect(2) system call.  Define a thin compatibility shim here
 * so the code compiles identically on both a development Linux / macOS host
 * and inside the PS4 userland.
 * ---------------------------------------------------------------------- */
#ifdef SCE_KERNEL_MPROTECT_AVAILABLE
/*
 * Prototype provided by the PS4 SDK / libSceLibcInternal.
 * int sceKernelMprotect(void *addr, size_t len, int prot);
 */
extern int sceKernelMprotect(void *addr, size_t len, int prot);
#define ps4_mprotect(addr, len, prot) sceKernelMprotect((addr), (len), (prot))
#else
/* Standard POSIX mprotect – used on development / test hosts. */
#define ps4_mprotect(addr, len, prot) mprotect((addr), (len), (prot))
#endif

/* -------------------------------------------------------------------------
 * Page-size helpers (4 KiB is universal for x86_64)
 * ---------------------------------------------------------------------- */
#define PAGE_SIZE_4K ((size_t)0x1000)

static inline size_t page_align(size_t v)
{
    return (v + PAGE_SIZE_4K - 1) & ~(PAGE_SIZE_4K - 1);
}

/* -------------------------------------------------------------------------
 * Function-pointer type for the payload entry point
 * ---------------------------------------------------------------------- */
typedef int (*payload_entry_t)(void);

/* -------------------------------------------------------------------------
 * test_macho_execution()
 * ---------------------------------------------------------------------- */
int test_macho_execution(const uint8_t *macho_data, size_t size)
{
    /* 1. Map and parse the Mach-O image. */
    void *entry_addr = load_mach_o_segments(macho_data, size);

    if (entry_addr == NULL) {
        /* Failed to parse the image or locate the entry point. */
        return 0;
    }

    /*
     * 2. Promote the mapped pages from RW to RX.
     *
     * mmap() returns pages that are readable and writable but NOT
     * executable.  On x86_64 hardware with NX enforcement (and on PS4 with
     * DEP active), attempting to execute RW memory triggers a page fault.
     * We must therefore call mprotect (or sceKernelMprotect on PS4) to
     * replace PROT_WRITE with PROT_EXEC before the jump.
     *
     * The segment size is approximated here as the page-aligned region that
     * starts at the beginning of the page containing entry_addr.  A full
     * implementation would track the exact mapping bounds returned by
     * load_mach_o_segments(); this simplification is sufficient for the PoC.
     */
    void   *page_base = (void *)((uintptr_t)entry_addr & ~(PAGE_SIZE_4K - 1));
    size_t  prot_size = page_align(size);   /* conservative upper bound */

    if (ps4_mprotect(page_base, prot_size,
                     VM_PROT_READ | VM_PROT_EXEC) != 0) {
        /* Could not change protection – abort rather than segfault. */
        return 0;
    }

    /*
     * 3. Memory fence.
     *
     * Although x86_64 has a strongly-ordered memory model and the I-cache
     * is coherent with the D-cache, an explicit store-fence (MFENCE) is
     * issued here to:
     *   a) prevent the compiler from reordering the memcpy (inside
     *      load_mach_o_segments) past the mprotect call, and
     *   b) demonstrate the correct practice for systems programming
     *      contexts where the target may have a weaker memory model.
     */
    __builtin_ia32_mfence();

    /*
     * 4. The leap of faith – execute the payload.
     *
     * The C standard does not allow casting between data and function
     * pointers directly; the double cast through uintptr_t is the
     * accepted portable workaround.
     */
    payload_entry_t execute_payload =
        (payload_entry_t)(uintptr_t)entry_addr;

    int result = execute_payload();

    /*
     * 5. Validate the result.
     *
     * payload.c computes 0x1337 + 0x42.  Any other value means the Mach-O
     * was not mapped or executed correctly.
     */
    if (result == 0x1379) {
        /* SUCCESS: the Mach-O was mapped and executed correctly. */
        return 1;
    }

    return 0;
}
