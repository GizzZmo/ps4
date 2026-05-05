/*
 * iso_loader.c - ISO 9660 filesystem and El Torito boot-image loader for PS4.
 *
 * Implements the three public functions declared in iso_loader.h:
 *
 *   iso_load_boot_image()  — parse an El Torito boot catalog and return a
 *                            malloc'd copy of the default boot image.
 *   iso_find_file()        — traverse the ISO 9660 directory tree and return
 *                            a direct pointer to a file's data.
 *   iso_free_boot_image()  — release memory allocated by iso_load_boot_image.
 *
 * Design notes
 * ------------
 * All input is treated as untrusted.  Every LBA, offset, and length value is
 * range-checked against iso_size before any pointer is formed.  No pointer
 * arithmetic produces a value outside the [iso_data, iso_data+iso_size) range.
 *
 * Only the little-endian halves of ISO 9660 both-byte-order (BBO) fields are
 * read.  The code compiles and runs correctly on both little-endian hosts
 * (x86-64) and the PS4 (FreeBSD/x86-64).
 */

#include "iso_loader.h"

#include <ctype.h>    /* toupper */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>   /* malloc, free  */
#include <string.h>   /* memcpy, memset, strncmp */

/* -------------------------------------------------------------------------
 * Internal helpers
 * ---------------------------------------------------------------------- */

/*
 * sector_ptr() — Return a pointer to the first byte of sector 'lba' within
 * the image, or NULL if the sector would be partially or fully outside the
 * buffer.
 */
static const uint8_t *sector_ptr(const uint8_t *iso, size_t iso_size,
                                  uint32_t lba)
{
    uint64_t off = (uint64_t)lba * ISO_SECTOR_SIZE;
    if (off + ISO_SECTOR_SIZE > (uint64_t)iso_size) {
        return NULL;
    }
    return iso + (size_t)off;
}

/*
 * istrncmp_upper() — Compare at most 'n' characters of 'a' against 'b',
 * converting each character of 'a' to upper case before comparison.  'b' is
 * expected to already be upper case (as ISO 9660 filenames are).
 * Returns 0 if the strings are equal over 'n' characters, non-zero otherwise.
 */
static int istrncmp_upper(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        unsigned char ca = (unsigned char)toupper((unsigned char)a[i]);
        unsigned char cb = (unsigned char)b[i];
        if (ca != cb) {
            return (int)ca - (int)cb;
        }
        if (ca == '\0') {
            return 0;
        }
    }
    return 0;
}

/*
 * stripped_name_len() — Return the length of an ISO 9660 file name after
 * stripping the ";N" version suffix appended by the disc mastering tool.
 * For example "VMLINUZ;1" returns 7.
 */
static uint8_t stripped_name_len(const char *name, uint8_t raw_len)
{
    for (uint8_t i = 0; i < raw_len; i++) {
        if (name[i] == ';') {
            return i;
        }
    }
    return raw_len;
}

/* -------------------------------------------------------------------------
 * iso_load_boot_image()
 * ---------------------------------------------------------------------- */

int iso_load_boot_image(const uint8_t *iso_data, size_t iso_size,
                        iso_boot_image_t *out)
{
    if (iso_data == NULL || out == NULL ||
        iso_size < (uint64_t)(ISO_SYSTEM_AREA + 1) * ISO_SECTOR_SIZE) {
        return -1;
    }

    /* ------------------------------------------------------------------
     * Pass 1 — Walk volume descriptors (starting at sector 16) to find
     * the El Torito Boot Record Volume Descriptor.
     * ---------------------------------------------------------------- */
    uint32_t boot_catalog_lba = 0;
    int      found = 0;

    for (uint32_t lba = ISO_SYSTEM_AREA; ; lba++) {
        const uint8_t *sec = sector_ptr(iso_data, iso_size, lba);
        if (sec == NULL) {
            break;
        }

        const iso_vd_header_t *hdr = (const iso_vd_header_t *)sec;

        /* All descriptors must carry the "CD001" identifier. */
        if (hdr->identifier[0] != 'C' || hdr->identifier[1] != 'D' ||
            hdr->identifier[2] != '0' || hdr->identifier[3] != '0' ||
            hdr->identifier[4] != '1') {
            break;
        }

        if (hdr->type == ISO_VD_TERMINATOR) {
            break;
        }

        if (hdr->type == ISO_VD_BOOT_RECORD) {
            const iso_boot_record_t *br = (const iso_boot_record_t *)sec;
            /* Verify the El Torito signature (first 23 chars are required). */
            if (strncmp(br->boot_system_id,
                        "EL TORITO SPECIFICATION", 23) == 0) {
                boot_catalog_lba = br->boot_catalog_lba;
                found = 1;
                /* Continue scanning in case a better descriptor follows;
                 * in practice there is only one Boot Record. */
            }
        }
    }

    if (!found) {
        return -2;
    }

    /* ------------------------------------------------------------------
     * Pass 2 — Read and validate the El Torito boot catalog.
     * ---------------------------------------------------------------- */
    const uint8_t *catalog = sector_ptr(iso_data, iso_size, boot_catalog_lba);
    if (catalog == NULL) {
        return -3;
    }

    /* Validation entry must fit inside the sector. */
    if (sizeof(iso_eltorito_validation_t) > ISO_SECTOR_SIZE) {
        return -3; /* should never happen; defensive guard */
    }

    const iso_eltorito_validation_t *ve =
        (const iso_eltorito_validation_t *)catalog;

    if (ve->header_id != 0x01 ||
        ve->key[0] != 0x55 || ve->key[1] != 0xAA) {
        return -3;
    }

    /* Boot entry immediately follows the validation entry. */
    if (sizeof(iso_eltorito_validation_t) +
            sizeof(iso_eltorito_entry_t) > ISO_SECTOR_SIZE) {
        return -3;
    }

    const iso_eltorito_entry_t *entry =
        (const iso_eltorito_entry_t *)(catalog +
                                       sizeof(iso_eltorito_validation_t));

    if (entry->boot_indicator != ISO_ELTORITO_BOOTABLE) {
        return -4;
    }

    /* ------------------------------------------------------------------
     * Pass 3 — Copy the boot image into a heap buffer.
     *
     * sector_count is in units of 512-byte virtual sectors.  For no-
     * emulation images this is the number of 512-byte chunks to load.
     * If sector_count is 0 (some mastering tools leave it zero), fall
     * back to one full ISO sector (2048 bytes).
     * ---------------------------------------------------------------- */
    uint32_t img_lba  = entry->load_rba;
    size_t   img_size = (entry->sector_count > 0)
                        ? (size_t)entry->sector_count * 512u
                        : ISO_SECTOR_SIZE;

    uint64_t img_off = (uint64_t)img_lba * ISO_SECTOR_SIZE;
    if (img_off + img_size > (uint64_t)iso_size) {
        return -5;
    }

    uint8_t *buf = malloc(img_size);
    if (buf == NULL) {
        return -6;
    }

    memcpy(buf, iso_data + (size_t)img_off, img_size);

    out->data        = buf;
    out->size        = img_size;
    out->platform_id = ve->platform_id;
    out->media_type  = entry->media_type;
    out->load_rba    = img_lba;
    return 0;
}

/* -------------------------------------------------------------------------
 * Directory helpers used by iso_find_file()
 * ---------------------------------------------------------------------- */

/*
 * find_in_dir() — Search a single ISO 9660 directory extent for an entry
 * whose name matches 'component' (stripped of any ";N" version suffix and
 * compared case-insensitively).
 *
 * If want_directory is non-zero, only directory entries (flags bit 1 set)
 * are matched; otherwise only file entries are matched.
 *
 * Returns a pointer to the matching iso_dir_record_t inside iso_data,
 * or NULL if not found.
 */
static const iso_dir_record_t *find_in_dir(const uint8_t *iso_data,
                                            size_t         iso_size,
                                            uint32_t       dir_lba,
                                            uint32_t       dir_size,
                                            const char    *component,
                                            size_t         comp_len,
                                            int            want_directory)
{
    uint64_t dir_off = (uint64_t)dir_lba * ISO_SECTOR_SIZE;
    if (dir_off + dir_size > (uint64_t)iso_size) {
        return NULL;
    }

    const uint8_t *ptr = iso_data + (size_t)dir_off;
    const uint8_t *end = ptr + dir_size;

    while (ptr < end) {
        /* Need at least the fixed part of the directory record. */
        if ((size_t)(end - ptr) < sizeof(iso_dir_record_t)) {
            break;
        }

        const iso_dir_record_t *rec = (const iso_dir_record_t *)ptr;

        if (rec->length == 0) {
            /*
             * A zero-length record signals the end of entries in this
             * logical sector.  Advance to the next sector boundary.
             */
            size_t pos  = (size_t)(ptr - iso_data);
            size_t next = (pos / ISO_SECTOR_SIZE + 1) * ISO_SECTOR_SIZE;
            if (next >= (size_t)(iso_data + iso_size - iso_data)) {
                break;
            }
            ptr = iso_data + next;
            continue;
        }

        /* Guard against a malformed length walking past end-of-directory. */
        if (ptr + rec->length > end) {
            break;
        }

        /* Verify the name field fits within the declared record length. */
        if ((size_t)sizeof(iso_dir_record_t) + rec->name_length >
                rec->length) {
            ptr += rec->length;
            continue;
        }

        /*
         * Skip "." and ".." entries: their name_length is 1 and the single
         * name byte is 0x00 (dot) or 0x01 (dot-dot).
         */
        if (rec->name_length == 1) {
            uint8_t nb = (uint8_t)iso_dir_name(rec)[0];
            if (nb == 0x00 || nb == 0x01) {
                ptr += rec->length;
                continue;
            }
        }

        uint8_t nlen   = stripped_name_len(iso_dir_name(rec),
                                           rec->name_length);
        int     is_dir = (rec->flags & ISO_DIR_FLAG_DIRECTORY) != 0;

        if ((size_t)nlen == comp_len &&
            istrncmp_upper(component, iso_dir_name(rec), comp_len) == 0 &&
            is_dir == (want_directory ? 1 : 0)) {
            return rec;
        }

        ptr += rec->length;
    }

    return NULL;
}

/* -------------------------------------------------------------------------
 * iso_find_file()
 * ---------------------------------------------------------------------- */

const uint8_t *iso_find_file(const uint8_t *iso_data, size_t iso_size,
                              const char *path, size_t *out_size)
{
    if (iso_data == NULL || path == NULL || path[0] != '/') {
        return NULL;
    }

    if (iso_size < (uint64_t)(ISO_SYSTEM_AREA + 1) * ISO_SECTOR_SIZE) {
        return NULL;
    }

    /* ------------------------------------------------------------------
     * Find the Primary Volume Descriptor.
     * ---------------------------------------------------------------- */
    const iso_pvd_t *pvd = NULL;

    for (uint32_t lba = ISO_SYSTEM_AREA; ; lba++) {
        const uint8_t *sec = sector_ptr(iso_data, iso_size, lba);
        if (sec == NULL) {
            break;
        }

        const iso_vd_header_t *hdr = (const iso_vd_header_t *)sec;

        if (hdr->identifier[0] != 'C' || hdr->identifier[1] != 'D' ||
            hdr->identifier[2] != '0' || hdr->identifier[3] != '0' ||
            hdr->identifier[4] != '1') {
            break;
        }

        if (hdr->type == ISO_VD_TERMINATOR) {
            break;
        }

        if (hdr->type == ISO_VD_PRIMARY) {
            pvd = (const iso_pvd_t *)sec;
            break;
        }
    }

    if (pvd == NULL) {
        return NULL;
    }

    /* ------------------------------------------------------------------
     * Start traversal from the root directory record embedded in the PVD.
     * ---------------------------------------------------------------- */
    const iso_dir_record_t *root =
        (const iso_dir_record_t *)pvd->root_dir_record;

    uint32_t cur_lba  = root->extent_lba.le;
    uint32_t cur_size = root->data_length.le;

    /* ------------------------------------------------------------------
     * Walk each path component, descending into sub-directories.
     * ---------------------------------------------------------------- */
    const char *p = path + 1; /* skip the leading '/' */

    while (*p != '\0') {
        /* Locate the end of this component. */
        const char *slash = p;
        while (*slash != '/' && *slash != '\0') {
            slash++;
        }

        size_t comp_len = (size_t)(slash - p);
        if (comp_len == 0) {
            /* Consecutive slashes or trailing slash — advance and continue. */
            p = slash + (*slash == '/' ? 1 : 0);
            continue;
        }

        int is_last = (*slash == '\0');

        const iso_dir_record_t *rec =
            find_in_dir(iso_data, iso_size, cur_lba, cur_size,
                        p, comp_len,
                        !is_last /* want_directory if not the last component */);

        if (rec == NULL) {
            return NULL;
        }

        if (!is_last) {
            /* Descend into the matching sub-directory. */
            cur_lba  = rec->extent_lba.le;
            cur_size = rec->data_length.le;
        } else {
            /* Found the target file. */
            uint64_t file_off = (uint64_t)rec->extent_lba.le * ISO_SECTOR_SIZE;
            uint32_t file_len = rec->data_length.le;

            if (file_off + file_len > (uint64_t)iso_size) {
                return NULL;
            }

            if (out_size != NULL) {
                *out_size = (size_t)file_len;
            }

            return iso_data + (size_t)file_off;
        }

        p = (*slash == '/') ? slash + 1 : slash;
    }

    return NULL;
}

/* -------------------------------------------------------------------------
 * iso_free_boot_image()
 * ---------------------------------------------------------------------- */

void iso_free_boot_image(iso_boot_image_t *img)
{
    if (img != NULL) {
        free(img->data);
        img->data = NULL;
        img->size = 0;
    }
}
