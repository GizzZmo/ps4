/*
 * iso_loader.h - ISO 9660 filesystem and El Torito boot-image loader for PS4.
 *
 * Parses an ISO 9660 disc image held in memory, locates the El Torito default
 * boot image, and provides a file-lookup API for traversing the ISO 9660
 * directory tree.  Intended as a building block for installing or booting an
 * operating system (e.g. a Linux distribution) on the PS4 directly from a
 * standard disc image.
 *
 * Supported formats
 * -----------------
 *   ISO 9660 Primary Volume Descriptor (sector 17 by convention)
 *   El Torito Boot Record (BIOS and EFI entries; no-emulation and floppy-
 *   emulation media types)
 *
 * Only the little-endian halves of both-byte-order (BBO) fields are read.
 * No external dependencies beyond <stddef.h>, <stdint.h>, and the C library
 * functions malloc/free/memcpy/memset/strlen.
 *
 * Official Darwin / macOS SDK resources
 * --------------------------------------
 * To cross-compile Mach-O x86 / x86_64 binaries targeting Darwin:
 *
 *  - Xcode (macOS native):
 *      https://developer.apple.com/download/applications/
 *
 *  - Xcode Command Line Tools (macOS native, no full IDE):
 *      https://developer.apple.com/download/all/?q=command+line+tools
 *
 *  - Apple Open Source (XNU / Darwin kernel source):
 *      https://opensource.apple.com/
 *
 *  - XNU source on GitHub (Apple OSS Distributions):
 *      https://github.com/apple-oss-distributions/xnu
 *
 *  - osxcross — macOS cross-compilation toolchain for Linux:
 *      https://github.com/tpoechtrager/osxcross
 */

#ifndef ISO_LOADER_H
#define ISO_LOADER_H

#include <stddef.h>
#include <stdint.h>

/* -------------------------------------------------------------------------
 * ISO 9660 constants
 * ---------------------------------------------------------------------- */

#define ISO_SECTOR_SIZE    2048u   /* bytes per logical block (standard)    */
#define ISO_SYSTEM_AREA    16u     /* first 16 sectors are reserved          */

/* Volume Descriptor type codes (byte 0 of each VD sector) */
#define ISO_VD_BOOT_RECORD   0u    /* Boot Record (e.g. El Torito)           */
#define ISO_VD_PRIMARY       1u    /* Primary Volume Descriptor              */
#define ISO_VD_SUPPLEMENTARY 2u    /* Supplementary / Joliet                 */
#define ISO_VD_PARTITION     3u    /* Volume Partition Descriptor            */
#define ISO_VD_TERMINATOR  255u    /* Volume Descriptor Set Terminator       */

/* El Torito platform IDs (validation entry byte 1) */
#define ISO_ELTORITO_PLATFORM_X86  0x00u  /* BIOS / IA-32 / x86-64           */
#define ISO_ELTORITO_PLATFORM_PPC  0x01u  /* PowerPC                         */
#define ISO_ELTORITO_PLATFORM_MAC  0x02u  /* Mac (old-world ROM)             */
#define ISO_ELTORITO_PLATFORM_EFI  0xefu  /* UEFI (modern x86-64 boot)       */

/* El Torito media types (initial entry byte 1) */
#define ISO_ELTORITO_MEDIA_NO_EMUL   0u   /* no-emulation (typical for modern ISOs) */
#define ISO_ELTORITO_MEDIA_FLOPPY_12 1u   /* 1.2 MB floppy emulation                */
#define ISO_ELTORITO_MEDIA_FLOPPY_14 2u   /* 1.44 MB floppy emulation               */
#define ISO_ELTORITO_MEDIA_FLOPPY_28 3u   /* 2.88 MB floppy emulation               */
#define ISO_ELTORITO_MEDIA_HD        4u   /* hard disk emulation                    */

/* El Torito boot indicator values */
#define ISO_ELTORITO_BOOTABLE      0x88u  /* entry is bootable                      */
#define ISO_ELTORITO_NOT_BOOTABLE  0x00u  /* entry is not bootable                  */

/* ISO 9660 directory record flag bits */
#define ISO_DIR_FLAG_HIDDEN    0x01u
#define ISO_DIR_FLAG_DIRECTORY 0x02u
#define ISO_DIR_FLAG_ASSOC     0x04u
#define ISO_DIR_FLAG_EXTENDED  0x08u
#define ISO_DIR_FLAG_PERMS     0x10u
#define ISO_DIR_FLAG_MULTI     0x80u   /* not the final record for this file     */

/* -------------------------------------------------------------------------
 * On-disc structures
 *
 * ISO 9660 uses "both-byte-order" (BBO) fields that store the same value
 * twice: once in little-endian and once in big-endian.  We expose only the
 * LE half via the .le member; the BE half (.be) is present for completeness.
 *
 * __attribute__((packed)) is used to guarantee the compiler does not insert
 * padding into these structures, which must exactly match the on-disc layout.
 * ---------------------------------------------------------------------- */

/* 16-bit BBO field (4 bytes on disc) */
typedef struct {
    uint16_t le;
    uint16_t be;
} __attribute__((packed)) iso16_t;

/* 32-bit BBO field (8 bytes on disc) */
typedef struct {
    uint32_t le;
    uint32_t be;
} __attribute__((packed)) iso32_t;

/* Generic Volume Descriptor header (common prefix of every VD sector) */
typedef struct {
    uint8_t  type;           /* ISO_VD_*                               */
    char     identifier[5];  /* always "CD001"                         */
    uint8_t  version;        /* always 0x01                            */
} __attribute__((packed)) iso_vd_header_t;

/*
 * Primary Volume Descriptor (type = 1, one full 2048-byte sector).
 *
 * Only the fields used by this loader are annotated below; the rest are
 * present for layout accuracy.  Total size is exactly ISO_SECTOR_SIZE bytes.
 */
typedef struct {
    uint8_t  type;                          /* 0x01                          */
    char     identifier[5];                 /* "CD001"                       */
    uint8_t  version;                       /* 0x01                          */
    uint8_t  _unused1;
    char     system_id[32];
    char     volume_id[32];
    uint8_t  _unused2[8];
    iso32_t  volume_space_size;             /* total logical blocks          */
    uint8_t  _unused3[32];
    iso16_t  volume_set_size;
    iso16_t  volume_sequence_number;
    iso16_t  logical_block_size;            /* usually 2048                  */
    iso32_t  path_table_size;
    uint32_t lba_path_table_le;             /* LBA of LE path table          */
    uint32_t lba_path_table_le_opt;
    uint32_t lba_path_table_be;             /* LBA of BE path table          */
    uint32_t lba_path_table_be_opt;
    uint8_t  root_dir_record[34];           /* embedded root directory record */
    char     volume_set_id[128];
    char     publisher_id[128];
    char     data_preparer_id[128];
    char     application_id[128];
    char     copyright_file_id[37];
    char     abstract_file_id[37];
    char     bibliographic_id[37];
    uint8_t  creation_date[17];
    uint8_t  modification_date[17];
    uint8_t  expiration_date[17];
    uint8_t  effective_date[17];
    uint8_t  file_structure_version;        /* 0x01                          */
    uint8_t  _unused4;
    uint8_t  application_use[512];
    uint8_t  _reserved[653];
} __attribute__((packed)) iso_pvd_t;

/*
 * Boot Record Volume Descriptor (type = 0, one full 2048-byte sector).
 * For El Torito discs, boot_system_id begins with "EL TORITO SPECIFICATION".
 */
typedef struct {
    uint8_t  type;                /* 0x00                                    */
    char     identifier[5];       /* "CD001"                                 */
    uint8_t  version;             /* 0x01                                    */
    char     boot_system_id[32];  /* "EL TORITO SPECIFICATION"               */
    uint8_t  _unused[32];
    uint32_t boot_catalog_lba;    /* LBA of the El Torito boot catalog       */
    uint8_t  _padding[1973];      /* pad to ISO_SECTOR_SIZE                  */
} __attribute__((packed)) iso_boot_record_t;

/*
 * ISO 9660 Directory Record (variable length; the name field follows the
 * fixed portion immediately).  Use iso_dir_name() to access the name.
 *
 * Note: record length is always rounded up to an even number of bytes to
 * maintain word alignment within a directory extent.
 */
typedef struct {
    uint8_t  length;                     /* total byte length of this record */
    uint8_t  ext_attr_length;
    iso32_t  extent_lba;                 /* LBA of the file / directory data */
    iso32_t  data_length;                /* byte size of the extent          */
    uint8_t  recording_date[7];
    uint8_t  flags;                      /* ISO_DIR_FLAG_*                   */
    uint8_t  interleave_file_unit_size;
    uint8_t  interleave_gap_size;
    iso16_t  volume_sequence_number;
    uint8_t  name_length;
    /* char name[name_length] immediately follows; access via iso_dir_name() */
} __attribute__((packed)) iso_dir_record_t;

/* -------------------------------------------------------------------------
 * El Torito boot-catalog structures (each entry is exactly 32 bytes)
 * ---------------------------------------------------------------------- */

/*
 * Validation Entry — mandatory first 32 bytes of the boot catalog.
 * The two's-complement sum of all 16-bit words in this entry must equal 0.
 */
typedef struct {
    uint8_t  header_id;        /* must be 0x01                              */
    uint8_t  platform_id;      /* ISO_ELTORITO_PLATFORM_*                   */
    uint16_t _reserved;
    char     id_string[24];    /* manufacturer / author string (informational) */
    uint16_t checksum;         /* chosen so the word-sum of the entry is 0  */
    uint8_t  key[2];           /* must be { 0x55, 0xAA }                   */
} __attribute__((packed)) iso_eltorito_validation_t;

/*
 * Initial / Default Boot Entry — the 32 bytes immediately following the
 * validation entry in the boot catalog.  Also used for Section Entries
 * within an El Torito Section Header.
 */
typedef struct {
    uint8_t  boot_indicator;   /* ISO_ELTORITO_BOOTABLE or NOT_BOOTABLE     */
    uint8_t  media_type;       /* ISO_ELTORITO_MEDIA_*                      */
    uint16_t load_segment;     /* x86 load segment; 0 → 0x07C0              */
    uint8_t  system_type;      /* MBR partition type byte (0 = no emul.)    */
    uint8_t  _unused;
    uint16_t sector_count;     /* virtual 512-byte sectors to load          */
    uint32_t load_rba;         /* LBA of the boot image on the disc         */
    uint8_t  _reserved[20];
} __attribute__((packed)) iso_eltorito_entry_t;

/*
 * Section Header Entry — follows the Initial/Default Boot Entry and each
 * subsequent set of Section Entries in an El Torito boot catalog.
 * header_indicator == 0x90 means more sections follow; 0x91 means this is
 * the final section header.
 */
#define ISO_ELTORITO_SECTION_MORE  0x90u  /* more sections follow           */
#define ISO_ELTORITO_SECTION_LAST  0x91u  /* last (or only) section header  */

typedef struct {
    uint8_t  header_indicator; /* ISO_ELTORITO_SECTION_MORE or _LAST        */
    uint8_t  platform_id;      /* ISO_ELTORITO_PLATFORM_*                   */
    uint16_t entry_count;      /* number of Section Entries that follow     */
    char     id_string[28];    /* informational section identifier           */
} __attribute__((packed)) iso_eltorito_section_header_t;

/* -------------------------------------------------------------------------
 * Public result structure
 * ---------------------------------------------------------------------- */

/*
 * iso_boot_image_t — describes a boot image extracted from an El Torito boot
 * catalog.  The 'data' pointer owns heap-allocated memory and must be
 * released by calling iso_free_boot_image().
 */
typedef struct {
    uint8_t *data;          /* malloc'd copy of the boot image bytes        */
    size_t   size;          /* byte length of the boot image                */
    uint8_t  platform_id;   /* ISO_ELTORITO_PLATFORM_* from validation entry */
    uint8_t  media_type;    /* ISO_ELTORITO_MEDIA_* from boot entry         */
    uint32_t load_rba;      /* source LBA in the ISO image                  */
} iso_boot_image_t;

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

/*
 * iso_load_boot_image() — Locate and extract the El Torito default boot image
 * from an ISO 9660 disc image held in memory.
 *
 * The function:
 *   1. Scans volume descriptors starting at sector 16 for an El Torito Boot
 *      Record Volume Descriptor.
 *   2. Reads and validates the El Torito boot catalog (validation entry key
 *      must be { 0x55, 0xAA }).
 *   3. Reads the Initial/Default Boot Entry; if it is not bootable, scans
 *      any Section Header / Section Entry records in the catalog for the
 *      first bootable entry.
 *   4. Copies the boot image bytes into a malloc'd buffer.
 *
 * Parameters:
 *   iso_data — pointer to the raw ISO image bytes in memory
 *   iso_size — byte length of the buffer
 *   out      — caller-supplied struct; filled on success
 *
 * Returns:
 *    0  success; out->data points to a malloc'd copy of the boot image.
 *              Call iso_free_boot_image(out) when finished.
 *   -1  iso_data is NULL, out is NULL, or iso_size is too small.
 *   -2  no El Torito Boot Record Volume Descriptor found.
 *   -3  boot catalog LBA is out of range or the validation entry is corrupt.
 *   -4  no bootable entry found in the boot catalog.
 *   -5  boot image extent is out of range.
 *   -6  malloc failed.
 */
int iso_load_boot_image(const uint8_t *iso_data, size_t iso_size,
                        iso_boot_image_t *out);

/*
 * iso_find_file() — Traverse the ISO 9660 directory tree and locate a file
 * by its absolute path.
 *
 * Parameters:
 *   iso_data — pointer to the raw ISO image bytes in memory
 *   iso_size — byte length of the buffer
 *   path     — absolute, slash-separated path, e.g. "/boot/vmlinuz".
 *              Each component is compared case-insensitively (ISO 9660 stores
 *              names in upper case).  Version suffixes (";N") are stripped
 *              from both the on-disc entry names and the caller-supplied
 *              path components before comparison, so "/VMLINUZ" and
 *              "/VMLINUZ;1" both match a disc entry named "VMLINUZ;1".
 *   out_size — if non-NULL, receives the file's byte length on success.
 *
 * Returns:
 *   A direct pointer into iso_data at the first byte of the file extent on
 *   success.  The pointer is valid for as long as iso_data remains valid; no
 *   allocation is performed.
 *   NULL if the file is not found, any parameter is invalid, or the extent
 *   lies outside iso_size.
 */
const uint8_t *iso_find_file(const uint8_t *iso_data, size_t iso_size,
                              const char *path, size_t *out_size);

/*
 * iso_free_boot_image() — Release the memory allocated by
 * iso_load_boot_image().  Safe to call with img == NULL or img->data == NULL.
 */
void iso_free_boot_image(iso_boot_image_t *img);

/*
 * iso_dir_name() — Return a pointer to the variable-length name field of a
 * directory record.  Convenience inline; avoids manual pointer arithmetic at
 * call sites.
 */
static inline const char *iso_dir_name(const iso_dir_record_t *rec)
{
    return (const char *)((const uint8_t *)rec + sizeof(iso_dir_record_t));
}

#endif /* ISO_LOADER_H */
