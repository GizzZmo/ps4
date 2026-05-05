/*
 * iso_loader_test.c - Self-contained test for the ISO 9660 / El Torito loader.
 *
 * Constructs a minimal, valid ISO 9660 + El Torito disc image entirely in
 * heap memory, then exercises iso_load_boot_image() and iso_find_file() on
 * it.  No disc file or cross-compilation toolchain is required; the test is
 * fully portable to any POSIX host.
 *
 * Disc layout used by the test (25 sectors = 51200 bytes):
 *
 *   Sector  0-15  System area (zeroed, reserved by ISO 9660)
 *   Sector 16     El Torito Boot Record VD  (boot_catalog_lba = 19)
 *   Sector 17     Primary Volume Descriptor (root directory at sector 20)
 *   Sector 18     Volume Descriptor Set Terminator
 *   Sector 19     El Torito boot catalog
 *                   Validation entry (32 bytes, platform = x86)
 *                   Initial boot entry (32 bytes, bootable, load_rba = 21)
 *   Sector 20     Root directory
 *                   "."       → sector 20, 2048 bytes, directory
 *                   ".."      → sector 20, 2048 bytes, directory
 *                   "TEST.TXT" → sector 22, 15 bytes, file
 *                   "BOOT"    → sector 23, 2048 bytes, directory
 *   Sector 21     Boot image (2048 bytes; first 4 are the x86 hang stub)
 *   Sector 22     File data: "Hello from ISO!" (15 bytes)
 *   Sector 23     BOOT/ subdirectory
 *                   "."       → sector 23, 2048 bytes, directory
 *                   ".."      → sector 20, 2048 bytes, directory
 *                   "VMLINUZ" → sector 24, 12 bytes, file
 *   Sector 24     VMLINUZ data: "LINUX_KERNEL" (12 bytes)
 *
 * Expected output:
 *   ISO loader test: PASS
 *
 * Exit code: 0 on success, 1 on any failure.
 */

#include "iso_loader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Test-image constants
 * ---------------------------------------------------------------------- */

#define TEST_NUM_SECTORS  25u
#define TEST_ISO_SIZE     ((size_t)TEST_NUM_SECTORS * ISO_SECTOR_SIZE)

#define LBA_ELTORITO_BRVD   16u
#define LBA_PVD             17u
#define LBA_VD_TERMINATOR   18u
#define LBA_BOOT_CATALOG    19u
#define LBA_ROOT_DIR        20u
#define LBA_BOOT_IMAGE      21u
#define LBA_TEST_TXT        22u
#define LBA_BOOT_SUBDIR     23u
#define LBA_VMLINUZ         24u

#define BOOT_IMAGE_SECTOR_COUNT  4u   /* 4 × 512 = 2048 bytes (1 ISO sector) */

static const uint8_t BOOT_IMAGE_MAGIC[4] = { 0xEB, 0xFE, 0x90, 0x00 };
static const char    FILE_TEST_TXT[]     = "Hello from ISO!";   /* 15 bytes */
static const char    FILE_VMLINUZ[]      = "LINUX_KERNEL";      /* 12 bytes */

/* -------------------------------------------------------------------------
 * Little-endian write helpers
 * ---------------------------------------------------------------------- */

static void put_u16le(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
}

static void put_u32le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
    p[2] = (uint8_t)((v >> 16) & 0xffu);
    p[3] = (uint8_t)((v >> 24) & 0xffu);
}

/* Write both halves of a BBO uint16 (little-endian then big-endian). */
static void put_bbo16(uint8_t *p, uint16_t v)
{
    put_u16le(p, v);
    p[2] = p[1]; p[3] = p[0]; /* big-endian in bytes 2-3 */
}

/* Write both halves of a BBO uint32. */
static void put_bbo32(uint8_t *p, uint32_t v)
{
    put_u32le(p, v);
    p[4] = p[3]; p[5] = p[2]; p[6] = p[1]; p[7] = p[0];
}

/* -------------------------------------------------------------------------
 * Directory record builder
 *
 * Writes a directory record into buf[0..record_len-1] and returns the padded
 * record length (always even).
 * ---------------------------------------------------------------------- */

static size_t make_dir_record(uint8_t  *buf,
                               uint32_t  extent_lba,
                               uint32_t  data_len,
                               uint8_t   flags,
                               const char *name,
                               uint8_t   name_len)
{
    /* Record length = 33 (fixed header) + name_length, rounded up to even. */
    size_t rec_len = 33u + name_len;
    if (rec_len & 1u) { rec_len++; }

    memset(buf, 0, rec_len);

    buf[0] = (uint8_t)rec_len;       /* DR length                           */
    buf[1] = 0u;                     /* extended attribute record length     */
    put_bbo32(buf + 2,  extent_lba); /* extent LBA  (BBO uint32, 8 bytes)   */
    put_bbo32(buf + 10, data_len);   /* data length (BBO uint32, 8 bytes)   */
    /* recording_date[7] at offset 18 — left as zeros (valid)               */
    buf[25] = flags;                 /* file flags                           */
    /* interleave fields at 26-27 — zero                                     */
    put_bbo16(buf + 28, 1u);         /* volume sequence number              */
    buf[32] = name_len;              /* name length                          */
    if (name_len > 0u) {
        memcpy(buf + 33, name, name_len);
    }

    return rec_len;
}

/* -------------------------------------------------------------------------
 * build_test_iso() — construct the in-memory disc image
 * ---------------------------------------------------------------------- */

/*
 * Shared helper: write the standard BRVD, PVD, VD Terminator, root directory
 * and boot image sectors into a pre-allocated (calloc'd) buffer.
 *
 * Parameters allow callers to control the boot catalog contents and
 * optional extra root-directory entries.
 *
 * 'extra_dir_off' / 'extra_dir_len': if non-zero, write an additional
 *   'extra_dir_len' bytes of pre-built directory record data at offset
 *   'extra_dir_off' within the root-directory sector.
 */
static void fill_common_sectors(uint8_t       *iso,
                                 uint32_t       boot_catalog_lba,
                                 uint32_t       root_dir_lba,
                                 uint32_t       boot_image_lba,
                                 uint16_t       catalog_sector_count,
                                 const uint8_t *extra_dir_data,
                                 size_t         extra_dir_off,
                                 size_t         extra_dir_len)
{
#define SEC(n) (iso + (size_t)(n) * ISO_SECTOR_SIZE)

    /* Sector 16 — El Torito Boot Record Volume Descriptor */
    {
        uint8_t *s = SEC(LBA_ELTORITO_BRVD);
        s[0] = ISO_VD_BOOT_RECORD;
        memcpy(s + 1, "CD001", 5);
        s[6] = 1u;
        memcpy(s + 7, "EL TORITO SPECIFICATION", 23);
        put_u32le(s + 71, boot_catalog_lba);
    }

    /* Sector 17 — Primary Volume Descriptor */
    {
        uint8_t *s = SEC(LBA_PVD);
        s[0] = ISO_VD_PRIMARY;
        memcpy(s + 1, "CD001", 5);
        s[6] = 1u;
        put_bbo32(s + 80, TEST_NUM_SECTORS);
        put_bbo16(s + 128, ISO_SECTOR_SIZE);
        make_dir_record(s + 156, root_dir_lba, ISO_SECTOR_SIZE,
                        ISO_DIR_FLAG_DIRECTORY, "\x00", 1u);
    }

    /* Sector 18 — Volume Descriptor Set Terminator */
    {
        uint8_t *s = SEC(LBA_VD_TERMINATOR);
        s[0] = ISO_VD_TERMINATOR;
        memcpy(s + 1, "CD001", 5);
        s[6] = 1u;
    }

    /* Boot catalog (validation + initial entry) */
    {
        uint8_t *s = SEC(boot_catalog_lba);
        s[0] = 0x01u;
        s[1] = ISO_ELTORITO_PLATFORM_X86;
        s[28] = 0xAAu;
        s[29] = 0x55u;
        s[30] = 0x55u;
        s[31] = 0xAAu;

        uint8_t *e = s + 32;
        e[0] = ISO_ELTORITO_BOOTABLE;
        e[1] = ISO_ELTORITO_MEDIA_NO_EMUL;
        put_u16le(e + 6, catalog_sector_count);
        put_u32le(e + 8, boot_image_lba);
    }

    /* Boot image sector */
    memcpy(SEC(boot_image_lba), BOOT_IMAGE_MAGIC, sizeof(BOOT_IMAGE_MAGIC));

    /* Root directory */
    {
        uint8_t *s   = SEC(root_dir_lba);
        size_t   off = 0;
        off += make_dir_record(s + off, root_dir_lba, ISO_SECTOR_SIZE,
                               ISO_DIR_FLAG_DIRECTORY, "\x00", 1u);
        off += make_dir_record(s + off, root_dir_lba, ISO_SECTOR_SIZE,
                               ISO_DIR_FLAG_DIRECTORY, "\x01", 1u);
        off += make_dir_record(s + off, LBA_TEST_TXT,
                               (uint32_t)(sizeof(FILE_TEST_TXT) - 1u),
                               0u, "TEST.TXT", 8u);
        off += make_dir_record(s + off, LBA_BOOT_SUBDIR, ISO_SECTOR_SIZE,
                               ISO_DIR_FLAG_DIRECTORY, "BOOT", 4u);

        if (extra_dir_data != NULL && extra_dir_len > 0 &&
                extra_dir_off + extra_dir_len <= ISO_SECTOR_SIZE) {
            memcpy(s + extra_dir_off, extra_dir_data, extra_dir_len);
        }

        (void)off;
    }

    /* TEST.TXT data */
    memcpy(SEC(LBA_TEST_TXT), FILE_TEST_TXT, sizeof(FILE_TEST_TXT) - 1u);

    /* BOOT/ subdirectory */
    {
        uint8_t *s   = SEC(LBA_BOOT_SUBDIR);
        size_t   off = 0;
        off += make_dir_record(s + off, LBA_BOOT_SUBDIR, ISO_SECTOR_SIZE,
                               ISO_DIR_FLAG_DIRECTORY, "\x00", 1u);
        off += make_dir_record(s + off, root_dir_lba, ISO_SECTOR_SIZE,
                               ISO_DIR_FLAG_DIRECTORY, "\x01", 1u);
        off += make_dir_record(s + off, LBA_VMLINUZ,
                               (uint32_t)(sizeof(FILE_VMLINUZ) - 1u),
                               0u, "VMLINUZ", 7u);
        (void)off;
    }

    /* VMLINUZ data */
    memcpy(SEC(LBA_VMLINUZ), FILE_VMLINUZ, sizeof(FILE_VMLINUZ) - 1u);

#undef SEC
}

/* -------------------------------------------------------------------------
 * build_test_iso() — construct the in-memory disc image
 * ---------------------------------------------------------------------- */

static uint8_t *build_test_iso(void)
{
    uint8_t *iso = calloc(1, TEST_ISO_SIZE);
    if (iso == NULL) {
        return NULL;
    }

    fill_common_sectors(iso,
                        LBA_BOOT_CATALOG, LBA_ROOT_DIR, LBA_BOOT_IMAGE,
                        (uint16_t)BOOT_IMAGE_SECTOR_COUNT,
                        NULL, 0, 0);
    return iso;
}

/*
 * build_section_catalog_iso() — ISO with a non-bootable Initial/Default entry
 * and a bootable EFI entry in the first catalog Section.
 *
 * Catalog layout (offsets within sector 19):
 *   0   Validation entry       (32 bytes)
 *   32  Initial entry          (32 bytes, NOT_BOOTABLE)
 *   64  Section Header         (32 bytes, entry_count = 1, LAST)
 *   96  Section Entry          (32 bytes, BOOTABLE, EFI platform)
 */
static uint8_t *build_section_catalog_iso(void)
{
    uint8_t *iso = calloc(1, TEST_ISO_SIZE);
    if (iso == NULL) {
        return NULL;
    }

    fill_common_sectors(iso,
                        LBA_BOOT_CATALOG, LBA_ROOT_DIR, LBA_BOOT_IMAGE,
                        (uint16_t)BOOT_IMAGE_SECTOR_COUNT,
                        NULL, 0, 0);

    /* Overwrite the catalog: non-bootable initial entry, bootable section. */
    uint8_t *cat = iso + (size_t)LBA_BOOT_CATALOG * ISO_SECTOR_SIZE;

    /* Initial entry: NOT_BOOTABLE */
    cat[32] = ISO_ELTORITO_NOT_BOOTABLE;

    /* Section Header at offset 64 */
    uint8_t *sh = cat + 64;
    sh[0] = ISO_ELTORITO_SECTION_LAST;      /* last section header        */
    sh[1] = ISO_ELTORITO_PLATFORM_EFI;      /* EFI platform               */
    put_u16le(sh + 2, 1u);                  /* entry_count = 1            */

    /* Section Entry at offset 96 */
    uint8_t *se = cat + 96;
    se[0] = ISO_ELTORITO_BOOTABLE;
    se[1] = ISO_ELTORITO_MEDIA_NO_EMUL;
    put_u16le(se + 6, (uint16_t)BOOT_IMAGE_SECTOR_COUNT);
    put_u32le(se + 8, LBA_BOOT_IMAGE);

    return iso;
}

/*
 * build_dir_size_iso() — ISO where the boot image has a directory entry
 * (so find_extent_size_by_lba returns the full ISO sector size) but the
 * catalog sector_count is set to 2 (= 1024 bytes < ISO_SECTOR_SIZE).
 * The loader should return img.size == ISO_SECTOR_SIZE.
 */
static uint8_t *build_dir_size_iso(void)
{
    uint8_t *iso = calloc(1, TEST_ISO_SIZE);
    if (iso == NULL) {
        return NULL;
    }

    /*
     * Build a directory record for "BOOTIMG.BIN" pointing to LBA_BOOT_IMAGE
     * with the full ISO_SECTOR_SIZE as data_length.
     * This record will be appended after "BOOT" in the root directory.
     */
    uint8_t extra_rec[44]; /* 33 (fixed header) + 11 (name) = 44, already even */
    memset(extra_rec, 0, sizeof(extra_rec));
    size_t extra_rec_len = make_dir_record(extra_rec,
                                           LBA_BOOT_IMAGE, ISO_SECTOR_SIZE,
                                           0u, "BOOTIMG.BIN", 11u);

    /*
     * Compute offset where extra record should go in the root-directory sector.
     * After "." (34) + ".." (34) + "TEST.TXT" (42) + "BOOT" (38) = 148 bytes.
     */
    size_t extra_off = 34u + 34u + 42u + 38u;

    fill_common_sectors(iso,
                        LBA_BOOT_CATALOG, LBA_ROOT_DIR, LBA_BOOT_IMAGE,
                        2u /* sector_count = 2 → load_size = 1024 */,
                        extra_rec, extra_off, extra_rec_len);
    return iso;
}

/* -------------------------------------------------------------------------
 * Test cases
 * ---------------------------------------------------------------------- */

static int run_tests(const uint8_t *iso, size_t iso_size)
{
    int pass = 1;

    /* --- Test 1: iso_load_boot_image() with NULL inputs --- */
    {
        iso_boot_image_t img;
        if (iso_load_boot_image(NULL, iso_size, &img) != -1) {
            fprintf(stderr, "FAIL: NULL iso_data should return -1\n");
            pass = 0;
        }
        if (iso_load_boot_image(iso, iso_size, NULL) != -1) {
            fprintf(stderr, "FAIL: NULL out should return -1\n");
            pass = 0;
        }
        if (iso_load_boot_image(iso, 0, &img) != -1) {
            fprintf(stderr, "FAIL: iso_size=0 should return -1\n");
            pass = 0;
        }
    }

    /* --- Test 2: iso_load_boot_image() success path --- */
    {
        iso_boot_image_t img;
        int rc = iso_load_boot_image(iso, iso_size, &img);
        if (rc != 0) {
            fprintf(stderr, "FAIL: iso_load_boot_image returned %d (expected 0)\n",
                    rc);
            pass = 0;
        } else {
            size_t expected_size = (size_t)BOOT_IMAGE_SECTOR_COUNT * 512u;
            if (img.size != expected_size) {
                fprintf(stderr,
                        "FAIL: boot image size %zu (expected %zu)\n",
                        img.size, expected_size);
                pass = 0;
            }
            if (img.data == NULL) {
                fprintf(stderr, "FAIL: boot image data is NULL\n");
                pass = 0;
            } else if (memcmp(img.data, BOOT_IMAGE_MAGIC,
                              sizeof(BOOT_IMAGE_MAGIC)) != 0) {
                fprintf(stderr, "FAIL: boot image magic bytes mismatch\n");
                pass = 0;
            }
            if (img.platform_id != ISO_ELTORITO_PLATFORM_X86) {
                fprintf(stderr, "FAIL: platform_id %u (expected %u)\n",
                        img.platform_id, ISO_ELTORITO_PLATFORM_X86);
                pass = 0;
            }
            if (img.media_type != ISO_ELTORITO_MEDIA_NO_EMUL) {
                fprintf(stderr, "FAIL: media_type %u (expected %u)\n",
                        img.media_type, ISO_ELTORITO_MEDIA_NO_EMUL);
                pass = 0;
            }
            if (img.load_rba != LBA_BOOT_IMAGE) {
                fprintf(stderr, "FAIL: load_rba %u (expected %u)\n",
                        img.load_rba, LBA_BOOT_IMAGE);
                pass = 0;
            }
            iso_free_boot_image(&img);
            if (img.data != NULL || img.size != 0) {
                fprintf(stderr,
                        "FAIL: iso_free_boot_image did not clear fields\n");
                pass = 0;
            }
        }
    }

    /* --- Test 3: iso_find_file() with invalid inputs --- */
    {
        size_t sz;
        if (iso_find_file(NULL, iso_size, "/TEST.TXT", &sz) != NULL) {
            fprintf(stderr, "FAIL: NULL iso_data should return NULL\n");
            pass = 0;
        }
        if (iso_find_file(iso, iso_size, NULL, &sz) != NULL) {
            fprintf(stderr, "FAIL: NULL path should return NULL\n");
            pass = 0;
        }
        if (iso_find_file(iso, iso_size, "no-leading-slash", &sz) != NULL) {
            fprintf(stderr,
                    "FAIL: path without leading '/' should return NULL\n");
            pass = 0;
        }
        if (iso_find_file(iso, iso_size, "/NONEXISTENT.BIN", &sz) != NULL) {
            fprintf(stderr, "FAIL: non-existent file should return NULL\n");
            pass = 0;
        }
    }

    /* --- Test 4: iso_find_file() — root-level file --- */
    {
        size_t           sz  = 0;
        const uint8_t   *ptr = iso_find_file(iso, iso_size, "/TEST.TXT", &sz);
        size_t expected_size = sizeof(FILE_TEST_TXT) - 1u;
        if (ptr == NULL) {
            fprintf(stderr, "FAIL: iso_find_file(\"/TEST.TXT\") returned NULL\n");
            pass = 0;
        } else if (sz != expected_size) {
            fprintf(stderr,
                    "FAIL: TEST.TXT size %zu (expected %zu)\n",
                    sz, expected_size);
            pass = 0;
        } else if (memcmp(ptr, FILE_TEST_TXT, expected_size) != 0) {
            fprintf(stderr, "FAIL: TEST.TXT content mismatch\n");
            pass = 0;
        }
    }

    /* --- Test 5: iso_find_file() — case-insensitive lookup --- */
    {
        size_t         sz  = 0;
        const uint8_t *ptr = iso_find_file(iso, iso_size, "/test.txt", &sz);
        if (ptr == NULL) {
            fprintf(stderr,
                    "FAIL: iso_find_file(\"/test.txt\") (lower-case) returned NULL\n");
            pass = 0;
        }
    }

    /* --- Test 6: iso_find_file() — file in subdirectory --- */
    {
        size_t           sz  = 0;
        const uint8_t   *ptr = iso_find_file(iso, iso_size,
                                              "/BOOT/VMLINUZ", &sz);
        size_t expected_size = sizeof(FILE_VMLINUZ) - 1u;
        if (ptr == NULL) {
            fprintf(stderr,
                    "FAIL: iso_find_file(\"/BOOT/VMLINUZ\") returned NULL\n");
            pass = 0;
        } else if (sz != expected_size) {
            fprintf(stderr,
                    "FAIL: VMLINUZ size %zu (expected %zu)\n",
                    sz, expected_size);
            pass = 0;
        } else if (memcmp(ptr, FILE_VMLINUZ, expected_size) != 0) {
            fprintf(stderr, "FAIL: VMLINUZ content mismatch\n");
            pass = 0;
        }
    }

    /* --- Test 7: iso_find_file() — mixed-case subdirectory path --- */
    {
        size_t         sz  = 0;
        const uint8_t *ptr = iso_find_file(iso, iso_size,
                                            "/boot/vmlinuz", &sz);
        if (ptr == NULL) {
            fprintf(stderr,
                    "FAIL: iso_find_file(\"/boot/vmlinuz\") (lower-case) "
                    "returned NULL\n");
            pass = 0;
        }
    }

    /* --- Test 8: iso_find_file() without out_size (NULL pointer) --- */
    {
        const uint8_t *ptr = iso_find_file(iso, iso_size, "/TEST.TXT", NULL);
        if (ptr == NULL) {
            fprintf(stderr,
                    "FAIL: iso_find_file with NULL out_size returned NULL\n");
            pass = 0;
        }
    }

    /* --- Test 9: iso_free_boot_image() with NULL and zeroed structs --- */
    {
        iso_free_boot_image(NULL); /* must not crash */

        iso_boot_image_t empty;
        memset(&empty, 0, sizeof(empty));
        iso_free_boot_image(&empty); /* must not crash */
    }

    /* --- Test 10: iso_find_file() — version suffix in caller path --- */
    {
        /* The on-disc entry is "TEST.TXT" (no suffix).  A caller path that
         * includes a version suffix ("/TEST.TXT;1") should still match. */
        size_t         sz  = 0;
        const uint8_t *ptr = iso_find_file(iso, iso_size, "/TEST.TXT;1", &sz);
        if (ptr == NULL) {
            fprintf(stderr,
                    "FAIL: iso_find_file(\"/TEST.TXT;1\") (version suffix) "
                    "returned NULL\n");
            pass = 0;
        }
    }

    return pass;
}

/* -------------------------------------------------------------------------
 * run_extra_tests() — tests that require separately built ISO images
 * ---------------------------------------------------------------------- */

static int run_extra_tests(void)
{
    int pass = 1;

    /* --- Test 11: multi-extent file returns NULL --- */
    {
        uint8_t *iso = build_test_iso();
        if (iso == NULL) {
            fprintf(stderr, "Cannot allocate ISO for multi-extent test\n");
            return 0;
        }

        /*
         * Patch the flags byte of the TEST.TXT directory record to set
         * ISO_DIR_FLAG_MULTI.  Within the root-directory sector the record
         * layout is: "."(34) + ".."(34) = 68 bytes before TEST.TXT.
         * The flags field is at byte 25 of the record (offset 68+25 = 93).
         */
        uint8_t *root_sec = iso + (size_t)LBA_ROOT_DIR * ISO_SECTOR_SIZE;
        root_sec[68 + 25] |= ISO_DIR_FLAG_MULTI;

        size_t         sz  = 0;
        const uint8_t *ptr = iso_find_file(iso, TEST_ISO_SIZE, "/TEST.TXT", &sz);
        if (ptr != NULL) {
            fprintf(stderr,
                    "FAIL: multi-extent file should return NULL\n");
            pass = 0;
        }

        free(iso);
    }

    /* --- Test 12: El Torito section-header scanning --- */
    {
        uint8_t *iso = build_section_catalog_iso();
        if (iso == NULL) {
            fprintf(stderr, "Cannot allocate ISO for section-catalog test\n");
            return 0;
        }

        iso_boot_image_t img;
        int rc = iso_load_boot_image(iso, TEST_ISO_SIZE, &img);
        if (rc != 0) {
            fprintf(stderr,
                    "FAIL: section-catalog ISO with non-bootable initial entry "
                    "returned %d (expected 0)\n", rc);
            pass = 0;
        } else {
            if (img.media_type != ISO_ELTORITO_MEDIA_NO_EMUL) {
                fprintf(stderr,
                        "FAIL: section entry media_type %u (expected %u)\n",
                        img.media_type, ISO_ELTORITO_MEDIA_NO_EMUL);
                pass = 0;
            }
            if (img.load_rba != LBA_BOOT_IMAGE) {
                fprintf(stderr,
                        "FAIL: section entry load_rba %u (expected %u)\n",
                        img.load_rba, LBA_BOOT_IMAGE);
                pass = 0;
            }
            iso_free_boot_image(&img);
        }

        free(iso);
    }

    /* --- Test 13: boot-image size resolved from directory entry --- */
    {
        uint8_t *iso = build_dir_size_iso();
        if (iso == NULL) {
            fprintf(stderr, "Cannot allocate ISO for dir-size test\n");
            return 0;
        }

        iso_boot_image_t img;
        int rc = iso_load_boot_image(iso, TEST_ISO_SIZE, &img);
        if (rc != 0) {
            fprintf(stderr,
                    "FAIL: dir-size ISO returned %d (expected 0)\n", rc);
            pass = 0;
        } else {
            /*
             * sector_count = 2 → load_size = 2 × 512 = 1024.
             * Directory entry data_length = ISO_SECTOR_SIZE = 2048.
             * The loader should use the larger directory size.
             */
            if (img.size != ISO_SECTOR_SIZE) {
                fprintf(stderr,
                        "FAIL: dir-size boot image size %zu (expected %u)\n",
                        img.size, ISO_SECTOR_SIZE);
                pass = 0;
            }
            iso_free_boot_image(&img);
        }

        free(iso);
    }

    return pass;
}

/* -------------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------- */

int main(void)
{
    uint8_t *iso = build_test_iso();
    if (iso == NULL) {
        fprintf(stderr, "Cannot allocate test ISO image\n");
        return 1;
    }

    int ok = run_tests(iso, TEST_ISO_SIZE);

    free(iso);

    ok &= run_extra_tests();

    printf("ISO loader test: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
