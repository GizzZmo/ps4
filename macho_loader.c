/*
 * macho_loader.c - Mach-O 64-bit segment loader for the PS4 PoC.
 *
 * Parses a raw Mach-O image held in memory, maps each LC_SEGMENT_64 into
 * anonymous (RW) pages, and returns a pointer to the binary's entry point
 * as determined by LC_MAIN or LC_UNIXTHREAD.
 *
 * Memory layout
 * -------------
 * A single contiguous mmap() reservation is made that covers the entire
 * virtual-address span of all PT_LOAD-equivalent segments.  Each segment's
 * raw bytes are then memcpy'd from the file image into the reservation at
 * the correct offset.  The caller is responsible for flipping the protection
 * to RX before jumping to the returned entry-point address.
 */

#include "macho_loader.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>    /* memcpy, memset */
#include <sys/mman.h>  /* mmap, munmap   */

/* -------------------------------------------------------------------------
 * Internal helpers
 * ---------------------------------------------------------------------- */

/*
 * Round v up to the next multiple of align.  align must be a power of two.
 */
static inline uint64_t align_up(uint64_t v, uint64_t align)
{
    return (v + align - 1) & ~(align - 1);
}

/* -------------------------------------------------------------------------
 * load_mach_o_segments()
 * ---------------------------------------------------------------------- */
void *load_mach_o_segments(const uint8_t *macho_data, size_t size,
                            void **out_map_base, size_t *out_map_size)
{
    const mach_header_64_t *hdr;
    const uint8_t          *cmd_ptr;
    uint32_t                i;

    /* Minimum size sanity check. */
    if (macho_data == NULL || size < sizeof(mach_header_64_t)) {
        return NULL;
    }

    hdr = (const mach_header_64_t *)macho_data;

    /* Validate the 64-bit Mach-O magic (native byte-order only). */
    if (hdr->magic != MH_MAGIC_64) {
        return NULL;
    }

    /* Verify the load-command region fits inside the supplied buffer. */
    if ((size_t)(sizeof(mach_header_64_t) + hdr->sizeofcmds) > size) {
        return NULL;
    }

    /* ------------------------------------------------------------------
     * First pass: compute the full vmaddr range of all LC_SEGMENT_64
     * commands so we can make a single contiguous mmap reservation.
     * ---------------------------------------------------------------- */
    uint64_t vm_min = UINT64_MAX;
    uint64_t vm_max = 0;

    cmd_ptr = macho_data + sizeof(mach_header_64_t);
    for (i = 0; i < hdr->ncmds; i++) {
        const load_command_t *lc = (const load_command_t *)cmd_ptr;

        /* Guard against a malformed cmdsize that would walk off the buffer. */
        if (lc->cmdsize < sizeof(load_command_t) ||
            (size_t)(cmd_ptr - macho_data) + lc->cmdsize >
                sizeof(mach_header_64_t) + hdr->sizeofcmds) {
            return NULL;
        }

        if (lc->cmd == LC_SEGMENT_64) {
            const segment_command_64_t *seg =
                (const segment_command_64_t *)cmd_ptr;

            if (seg->vmsize == 0) {
                cmd_ptr += lc->cmdsize;
                continue;
            }

            if (seg->vmaddr < vm_min) {
                vm_min = seg->vmaddr;
            }
            uint64_t seg_end = seg->vmaddr + seg->vmsize;
            if (seg_end > vm_max) {
                vm_max = seg_end;
            }
        }

        cmd_ptr += lc->cmdsize;
    }

    if (vm_min == UINT64_MAX || vm_max <= vm_min) {
        /* No loadable segments found. */
        return NULL;
    }

    /* ------------------------------------------------------------------
     * Allocate a contiguous RW region large enough to hold all segments.
     * We request MAP_FIXED_NOREPLACE so we don't silently stomp existing
     * mappings; fall back to a non-fixed mapping if the address is busy.
     * ---------------------------------------------------------------- */
    size_t   total_size = (size_t)align_up(vm_max - vm_min, 0x1000);
    uint64_t slide      = 0;  /* ASLR slide; computed after mmap */

#ifdef MAP_FIXED_NOREPLACE
    void *base = mmap((void *)vm_min, total_size,
                      PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
                      -1, 0);
    if (base == MAP_FAILED) {
        /* Preferred address busy – let the kernel pick a free address. */
        base = mmap(NULL, total_size,
                    PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS,
                    -1, 0);
    }
#else
    void *base = mmap(NULL, total_size,
                      PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS,
                      -1, 0);
#endif

    if (base == MAP_FAILED) {
        return NULL;
    }

    /* Zero the entire reservation (mmap of /dev/zero is already zeroed, but
     * be explicit for clarity). */
    memset(base, 0, total_size);

    slide = (uint64_t)(uintptr_t)base - vm_min;

    /* ------------------------------------------------------------------
     * Second pass: copy each segment's raw bytes from the file image into
     * the mapped region.
     * ---------------------------------------------------------------- */
    cmd_ptr = macho_data + sizeof(mach_header_64_t);
    for (i = 0; i < hdr->ncmds; i++) {
        const load_command_t *lc = (const load_command_t *)cmd_ptr;

        if (lc->cmd == LC_SEGMENT_64) {
            const segment_command_64_t *seg =
                (const segment_command_64_t *)cmd_ptr;

            if (seg->filesize > 0) {
                /* Validate source range fits inside the input buffer. */
                if ((uint64_t)seg->fileoff + seg->filesize > (uint64_t)size) {
                    munmap(base, total_size);
                    return NULL;
                }

                /* Validate destination fits inside the reservation. */
                uint8_t *dest = (uint8_t *)base +
                                (seg->vmaddr - vm_min);
                if ((uintptr_t)dest + seg->filesize >
                    (uintptr_t)base + total_size) {
                    munmap(base, total_size);
                    return NULL;
                }

                memcpy(dest, macho_data + seg->fileoff, (size_t)seg->filesize);
            }
        }

        cmd_ptr += lc->cmdsize;
    }

    /* ------------------------------------------------------------------
     * Third pass: locate the entry point.
     *
     *   LC_MAIN      – entryoff is a file-image offset from the start of
     *                  the __TEXT segment (i.e. from vm_min).
     *   LC_UNIXTHREAD – contains a full register state; for x86_64 the
     *                   RIP value in the thread state is the entry point.
     * ---------------------------------------------------------------- */
    void    *entry_addr = NULL;

    cmd_ptr = macho_data + sizeof(mach_header_64_t);
    for (i = 0; i < hdr->ncmds; i++) {
        const load_command_t *lc = (const load_command_t *)cmd_ptr;

        if (lc->cmd == LC_MAIN) {
            if (lc->cmdsize < sizeof(entry_point_command_t)) {
                munmap(base, total_size);
                return NULL;
            }
            const entry_point_command_t *ep =
                (const entry_point_command_t *)cmd_ptr;

            /*
             * entryoff is relative to the start of the mapped __TEXT
             * segment (vm_min after applying the slide).
             * Validate that it falls within the reservation before use.
             */
            if (ep->entryoff >= (uint64_t)total_size) {
                munmap(base, total_size);
                return NULL;
            }

            entry_addr = (void *)((uintptr_t)base + ep->entryoff);
            break;
        }

        if (lc->cmd == LC_UNIXTHREAD) {
            /*
             * LC_UNIXTHREAD layout for x86_64:
             *   Offset  Field
             *    +0     cmd       (uint32_t)
             *    +4     cmdsize   (uint32_t)
             *    +8     flavor    (uint32_t, x86_THREAD_STATE64 == 4)
             *    +12    count     (uint32_t, number of 32-bit words following)
             *    +16    x86_thread_state64 begins here
             *
             * x86_thread_state64 register order (each uint64_t, 8 bytes):
             *   index 0  rax
             *   index 1  rbx
             *   index 2  rcx
             *   index 3  rdx
             *   index 4  rdi
             *   index 5  rsi
             *   index 6  rbp
             *   index 7  rsp
             *   index 8  r8
             *   …
             *   index 15 r15
             *   index 16 rip  ← the entry-point address
             *
             * rip_offset = 16 (four uint32_t fields above) +
             *              16 * sizeof(uint64_t) (16 registers before rip)
             *            = 16 + 128 = 144 bytes from the start of the cmd.
             */
            const size_t rip_offset = 16 + 16 * sizeof(uint64_t);
            if (lc->cmdsize < rip_offset + sizeof(uint64_t)) {
                cmd_ptr += lc->cmdsize;
                continue;
            }

            uint64_t rip;
            memcpy(&rip, cmd_ptr + rip_offset, sizeof(rip));

            void *ep_addr = (void *)(uintptr_t)(rip + slide);

            /* Validate the computed entry point falls within the mapping. */
            if ((uintptr_t)ep_addr < (uintptr_t)base ||
                (uintptr_t)ep_addr >= (uintptr_t)base + total_size) {
                munmap(base, total_size);
                return NULL;
            }

            entry_addr = ep_addr;
            break;
        }

        cmd_ptr += lc->cmdsize;
    }

    if (entry_addr == NULL) {
        munmap(base, total_size);
        return NULL;
    }

    /* Provide the mapping coordinates to the caller for mprotect / munmap. */
    if (out_map_base != NULL) {
        *out_map_base = base;
    }
    if (out_map_size != NULL) {
        *out_map_size = total_size;
    }

    return entry_addr;
}
