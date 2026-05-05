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

static uint8_t *build_test_iso(void)
{
    uint8_t *iso = calloc(1, TEST_ISO_SIZE);
    if (iso == NULL) {
        return NULL;
    }

    /* Convenience pointer to the start of a given sector. */
#define SEC(n) (iso + (size_t)(n) * ISO_SECTOR_SIZE)

    /* ------------------------------------------------------------------
     * Sector 16 — El Torito Boot Record Volume Descriptor
     * ---------------------------------------------------------------- */
    {
        uint8_t *s = SEC(LBA_ELTORITO_BRVD);
        s[0] = ISO_VD_BOOT_RECORD;                 /* type             */
        memcpy(s + 1, "CD001", 5);                 /* identifier       */
        s[6] = 1u;                                 /* version          */
        memcpy(s + 7, "EL TORITO SPECIFICATION", 23); /* boot sys id   */
        /* boot_catalog_lba at offset 71 (32-bit LE) */
        put_u32le(s + 71, LBA_BOOT_CATALOG);
    }

    /* ------------------------------------------------------------------
     * Sector 17 — Primary Volume Descriptor
     * ---------------------------------------------------------------- */
    {
        uint8_t *s = SEC(LBA_PVD);
        s[0] = ISO_VD_PRIMARY;                     /* type             */
        memcpy(s + 1, "CD001", 5);                 /* identifier       */
        s[6] = 1u;                                 /* version          */
        /* system_id @ 8, volume_id @ 40 — left blank                   */
        /* volume_space_size (BBO uint32) @ 80 */
        put_bbo32(s + 80, TEST_NUM_SECTORS);
        /* logical_block_size (BBO uint16) @ 128 */
        put_bbo16(s + 128, ISO_SECTOR_SIZE);
        /* path table size (BBO uint32) @ 132 — irrelevant for this loader */
        /* root directory record @ offset 156 (34 bytes) */
        make_dir_record(s + 156,
                        LBA_ROOT_DIR, ISO_SECTOR_SIZE,
                        ISO_DIR_FLAG_DIRECTORY,
                        "\x00", 1u); /* root name is a single 0x00 byte */
    }

    /* ------------------------------------------------------------------
     * Sector 18 — Volume Descriptor Set Terminator
     * ---------------------------------------------------------------- */
    {
        uint8_t *s = SEC(LBA_VD_TERMINATOR);
        s[0] = ISO_VD_TERMINATOR;
        memcpy(s + 1, "CD001", 5);
        s[6] = 1u;
    }

    /* ------------------------------------------------------------------
     * Sector 19 — El Torito boot catalog
     *
     * Validation entry (32 bytes):
     *   Checksum is chosen so the 16-bit word sum of the 32-byte entry = 0.
     *   With header_id=0x01, platform=0x00, id_string=zeros, key={0x55,0xAA}:
     *     sum = 0x0001 (bytes 0-1) + 0xAA55 (bytes 30-31) = 0xAA56
     *     checksum = (0x10000 - 0xAA56) & 0xFFFF = 0x55AA
     *     stored LE: byte 28 = 0xAA, byte 29 = 0x55
     * ---------------------------------------------------------------- */
    {
        uint8_t *s = SEC(LBA_BOOT_CATALOG);

        /* Validation entry */
        s[0] = 0x01u;                    /* header_id                    */
        s[1] = ISO_ELTORITO_PLATFORM_X86;/* platform_id                  */
        /* bytes 2-3: reserved (zero) */
        /* bytes 4-27: id_string (zero) */
        s[28] = 0xAAu;                   /* checksum low byte            */
        s[29] = 0x55u;                   /* checksum high byte → 0x55AA  */
        s[30] = 0x55u;                   /* key[0]                       */
        s[31] = 0xAAu;                   /* key[1]                       */

        /* Initial/Default Boot Entry (immediately follows at offset 32) */
        uint8_t *e = s + 32;
        e[0] = ISO_ELTORITO_BOOTABLE;   /* boot_indicator               */
        e[1] = ISO_ELTORITO_MEDIA_NO_EMUL; /* media_type               */
        /* load_segment @ bytes 2-3: zero → 0x07C0 for x86              */
        /* system_type @ byte 4: zero                                    */
        put_u16le(e + 6, (uint16_t)BOOT_IMAGE_SECTOR_COUNT); /* sector_count */
        put_u32le(e + 8, LBA_BOOT_IMAGE);/* load_rba                    */
    }

    /* ------------------------------------------------------------------
     * Sector 21 — Boot image
     * ---------------------------------------------------------------- */
    {
        uint8_t *s = SEC(LBA_BOOT_IMAGE);
        memcpy(s, BOOT_IMAGE_MAGIC, sizeof(BOOT_IMAGE_MAGIC));
    }

    /* ------------------------------------------------------------------
     * Sector 20 — Root directory entries
     * ---------------------------------------------------------------- */
    {
        uint8_t *s   = SEC(LBA_ROOT_DIR);
        size_t   off = 0;

        /* "." */
        off += make_dir_record(s + off,
                               LBA_ROOT_DIR, ISO_SECTOR_SIZE,
                               ISO_DIR_FLAG_DIRECTORY, "\x00", 1u);

        /* ".." */
        off += make_dir_record(s + off,
                               LBA_ROOT_DIR, ISO_SECTOR_SIZE,
                               ISO_DIR_FLAG_DIRECTORY, "\x01", 1u);

        /* "TEST.TXT" — note: mastering tools append ";1", our loader strips it */
        off += make_dir_record(s + off,
                               LBA_TEST_TXT, sizeof(FILE_TEST_TXT) - 1u,
                               0u /* file */, "TEST.TXT", 8u);

        /* "BOOT" subdirectory */
        off += make_dir_record(s + off,
                               LBA_BOOT_SUBDIR, ISO_SECTOR_SIZE,
                               ISO_DIR_FLAG_DIRECTORY, "BOOT", 4u);

        (void)off; /* suppress unused-variable warning */
    }

    /* ------------------------------------------------------------------
     * Sector 22 — TEST.TXT file data
     * ---------------------------------------------------------------- */
    {
        uint8_t *s = SEC(LBA_TEST_TXT);
        memcpy(s, FILE_TEST_TXT, sizeof(FILE_TEST_TXT) - 1u);
    }

    /* ------------------------------------------------------------------
     * Sector 23 — BOOT/ subdirectory entries
     * ---------------------------------------------------------------- */
    {
        uint8_t *s   = SEC(LBA_BOOT_SUBDIR);
        size_t   off = 0;

        /* "." → this directory */
        off += make_dir_record(s + off,
                               LBA_BOOT_SUBDIR, ISO_SECTOR_SIZE,
                               ISO_DIR_FLAG_DIRECTORY, "\x00", 1u);

        /* ".." → root directory */
        off += make_dir_record(s + off,
                               LBA_ROOT_DIR, ISO_SECTOR_SIZE,
                               ISO_DIR_FLAG_DIRECTORY, "\x01", 1u);

        /* "VMLINUZ" */
        off += make_dir_record(s + off,
                               LBA_VMLINUZ, sizeof(FILE_VMLINUZ) - 1u,
                               0u /* file */, "VMLINUZ", 7u);

        (void)off;
    }

    /* ------------------------------------------------------------------
     * Sector 24 — VMLINUZ file data
     * ---------------------------------------------------------------- */
    {
        uint8_t *s = SEC(LBA_VMLINUZ);
        memcpy(s, FILE_VMLINUZ, sizeof(FILE_VMLINUZ) - 1u);
    }

#undef SEC
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

    printf("ISO loader test: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
