/* Tests for src/fat32ro.c against synthetic, hand-built FAT32 images
 * (a plain in-memory sector array plus a read_sectors callback) -- no
 * calculator, no real USB device, no real FAT32 volume needed. */
#include "../src/fat32ro.h"

#include <stdio.h>
#include <string.h>

static int g_failures = 0;
#define CHECK(cond, msg) \
    do { \
        if (!(cond)) { \
            printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
            g_failures++; \
        } \
    } while (0)

/* --- synthetic disk --------------------------------------------------- */

#define TEST_DISK_SECTORS 4608
static uint8_t g_disk[TEST_DISK_SECTORS][512];

static uint32_t disk_read(void *ctx, uint32_t lba, uint32_t count, void *buffer)
{
    uint32_t i;
    (void)ctx;

    for (i = 0; i < count; ++i) {
        if (lba + i >= TEST_DISK_SECTORS) {
            return i;
        }
        memcpy((uint8_t *)buffer + (size_t)i * 512, g_disk[lba + i], 512);
    }
    return count;
}

static uint32_t g_fail_calls;
static uint32_t disk_read_always_fails(void *ctx, uint32_t lba, uint32_t count, void *buffer)
{
    (void)ctx; (void)lba; (void)count; (void)buffer;
    g_fail_calls++;
    return 0;
}

static void put_u16le(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put_u32le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

typedef struct {
    uint32_t base_lba;
    uint16_t reserved_sectors;
    uint8_t sectors_per_cluster;
    uint8_t num_fats;
    uint32_t fat_size_sectors;
    uint32_t root_cluster;
    uint32_t total_sectors;
} fs_layout_t;

static uint32_t first_data_sector(const fs_layout_t *l)
{
    return l->base_lba + l->reserved_sectors + (uint32_t)l->num_fats * l->fat_size_sectors;
}

static uint32_t cluster_lba(const fs_layout_t *l, uint32_t cluster)
{
    return first_data_sector(l) + (cluster - 2) * (uint32_t)l->sectors_per_cluster;
}

static void write_boot_sector(const fs_layout_t *l)
{
    uint8_t *s = g_disk[l->base_lba];

    memset(s, 0, 512);
    s[0] = 0xEB; s[1] = 0x58; s[2] = 0x90;
    memcpy(s + 3, "MSWIN4.1", 8);
    put_u16le(s + 11, 512);
    s[13] = l->sectors_per_cluster;
    put_u16le(s + 14, l->reserved_sectors);
    s[16] = l->num_fats;
    put_u16le(s + 17, 0);
    put_u16le(s + 19, 0);
    s[21] = 0xF8;
    put_u16le(s + 22, 0);
    put_u32le(s + 32, l->total_sectors);
    put_u32le(s + 36, l->fat_size_sectors);
    put_u32le(s + 44, l->root_cluster);
    s[66] = 0x29;
    memcpy(s + 82, "FAT32   ", 8);
    s[510] = 0x55; s[511] = 0xAA;
}

static void set_fat_entry(const fs_layout_t *l, uint32_t cluster, uint32_t value)
{
    uint32_t first_fat_sector = l->base_lba + l->reserved_sectors;
    uint32_t fat_offset = cluster * 4;
    uint32_t sector = first_fat_sector + fat_offset / 512;
    uint32_t off = fat_offset % 512;

    put_u32le(g_disk[sector] + off, value & 0x0FFFFFFF);
}

/* name11 must be exactly 11 chars, space-padded, no dot (e.g. "MOVIE01 BIN"). */
static void write_dir_entry(const fs_layout_t *l, uint32_t dir_cluster, int index,
                             const char *name11, uint8_t attr,
                             uint32_t first_cluster, uint32_t file_size)
{
    uint8_t *entry = g_disk[cluster_lba(l, dir_cluster)] + index * 32;

    memcpy(entry, name11, 11);
    entry[11] = attr;
    put_u16le(entry + 20, (uint16_t)(first_cluster >> 16));
    put_u16le(entry + 26, (uint16_t)(first_cluster & 0xFFFFu));
    put_u32le(entry + 28, file_size);
}

static void reset_disk(void)
{
    memset(g_disk, 0, sizeof(g_disk));
}

/* A layout usable by most tests: 1 FAT, 1 reserved sector, 8
 * sectors/cluster, plenty of FAT table space for a few hundred clusters,
 * root directory occupying cluster 2 only (one 8-sector cluster, 128
 * entries -- ample for these tests). */
static fs_layout_t standard_layout(uint32_t base_lba)
{
    fs_layout_t l;

    l.base_lba = base_lba;
    l.reserved_sectors = 1;
    l.sectors_per_cluster = 8;
    l.num_fats = 1;
    l.fat_size_sectors = 4; /* 4*512/4 = 512 entries -- plenty for these tests */
    l.root_cluster = 2;
    l.total_sectors = base_lba + 1 + 4 + 8 * 300; /* room for ~300 data clusters */
    return l;
}

static void format_disk(const fs_layout_t *l)
{
    write_boot_sector(l);
    /* Root directory cluster's FAT entry: end-of-chain (single-cluster
     * root, sufficient for these tests). */
    set_fat_entry(l, l->root_cluster, 0x0FFFFFFF);
}

/* --- mount -------------------------------------------------------------- */

static void test_mount_superfloppy(void)
{
    fs_layout_t l = standard_layout(0);
    fat32ro_volume_t vol;

    reset_disk();
    format_disk(&l);

    CHECK(fat32ro_mount(&vol, disk_read, NULL) == FAT32RO_SUCCESS, "superfloppy mounts");
    CHECK(vol.partition_base_lba == 0, "base_lba is 0 for superfloppy");
    CHECK(vol.sectors_per_cluster == l.sectors_per_cluster, "sectors_per_cluster read correctly");
    CHECK(vol.root_cluster == l.root_cluster, "root_cluster read correctly");
    CHECK(vol.first_data_sector == first_data_sector(&l), "first_data_sector computed correctly");
}

static void test_mount_rejects_swapped_boot_signature(void)
{
    /* Regression test for a real bug: the boot signature is a fixed
     * BYTE SEQUENCE (0x55 at offset 510, then 0xAA at offset 511) per
     * the FAT spec -- it is not a little-endian-encoded 16-bit value
     * that happens to be named "0x55AA". Reading those two bytes with a
     * little-endian load actually yields 0xAA55, so comparing against
     * the literal 0x55AA constant rejected every real, correctly
     * formatted FAT32 boot sector (this is exactly what made a genuine
     * Windows-formatted FAT32 drive get reported as FAT32RO_NOT_FAT32
     * during real hardware testing). This test pins the correct byte
     * order and guards against reintroducing the swap. */
    fs_layout_t l = standard_layout(0);
    fat32ro_volume_t vol;
    uint8_t *s = g_disk[0];

    reset_disk();
    format_disk(&l);
    CHECK(fat32ro_mount(&vol, disk_read, NULL) == FAT32RO_SUCCESS,
          "sanity check: the correctly-ordered signature mounts");

    reset_disk();
    format_disk(&l);
    s[510] = 0xAA; s[511] = 0x55; /* the two signature bytes, swapped */
    CHECK(fat32ro_mount(&vol, disk_read, NULL) != FAT32RO_SUCCESS,
          "a swapped boot signature (0xAA,0x55) is rejected, not accepted");
}

static void test_mount_realistic_small_fat32_drive(void)
{
    /* Mirrors the real drive that exposed the signature-endianness bug:
     * a small (~245MB) USB drive, Windows `format /FS:FAT32` defaults --
     * 4 sectors/cluster (2048-byte clusters), 2 FATs, 32 reserved
     * sectors, superfloppy layout (no MBR). Exists so a future change
     * that happens to satisfy the narrower synthetic-test layouts above
     * but not a realistic one still gets caught. */
    fat32ro_volume_t vol;
    fs_layout_t l;

    l.base_lba = 0;
    l.reserved_sectors = 32;
    l.sectors_per_cluster = 4;
    l.num_fats = 2;
    l.fat_size_sectors = 500;
    l.root_cluster = 2;
    l.total_sectors = 501555; /* ~245MB at 512 bytes/sector */

    reset_disk();
    format_disk(&l);

    CHECK(fat32ro_mount(&vol, disk_read, NULL) == FAT32RO_SUCCESS,
          "a realistic small Windows-formatted FAT32 drive mounts");
    CHECK(vol.sectors_per_cluster == 4, "sectors_per_cluster matches a 2048-byte cluster");
}

static void test_mount_mbr_partition(void)
{
    const uint32_t partition_lba = 63; /* a plausible, non-zero partition start */
    fs_layout_t l = standard_layout(partition_lba);
    fat32ro_volume_t vol;
    uint8_t *mbr;

    reset_disk();
    format_disk(&l);

    mbr = g_disk[0];
    memset(mbr, 0, 512);
    /* One partition entry: type 0x0C (FAT32 LBA), starting at partition_lba. */
    mbr[446 + 4] = 0x0C;
    put_u32le(mbr + 446 + 8, partition_lba);
    put_u32le(mbr + 446 + 12, l.total_sectors - partition_lba);
    mbr[510] = 0x55; mbr[511] = 0xAA;

    CHECK(fat32ro_mount(&vol, disk_read, NULL) == FAT32RO_SUCCESS, "MBR-partitioned volume mounts");
    CHECK(vol.partition_base_lba == partition_lba, "base_lba resolved from the MBR partition entry");
}

static void test_mount_mbr_partition_with_stale_type_byte(void)
{
    /* Regression test for a real bug: a partition reformatted in place
     * (e.g. Windows' `format D: /FS:FAT32` run against an existing
     * volume, rather than a full repartition) rewrites the filesystem
     * inside the partition but doesn't necessarily update the MBR's
     * type byte to match -- so a real FAT32 partition can end up
     * sitting behind a stale, non-FAT32 type byte left over from
     * however the drive was originally partitioned. Mount must still
     * succeed by checking the partition's actual boot sector content,
     * not just trusting the declared type byte. */
    const uint32_t partition_lba = 63;
    fs_layout_t l = standard_layout(partition_lba);
    fat32ro_volume_t vol;
    uint8_t *mbr;

    reset_disk();
    format_disk(&l);

    mbr = g_disk[0];
    memset(mbr, 0, 512);
    mbr[446 + 4] = 0x07; /* stale NTFS/exFAT type byte -- the content is really FAT32 */
    put_u32le(mbr + 446 + 8, partition_lba);
    put_u32le(mbr + 446 + 12, l.total_sectors - partition_lba);
    mbr[510] = 0x55; mbr[511] = 0xAA;

    CHECK(fat32ro_mount(&vol, disk_read, NULL) == FAT32RO_SUCCESS,
          "a real FAT32 partition mounts even behind a stale/wrong MBR type byte");
    CHECK(vol.partition_base_lba == partition_lba,
          "base_lba still resolved correctly despite the wrong type byte");
}

static void test_mount_rejects_missing_signature(void)
{
    fat32ro_volume_t vol;

    reset_disk(); /* all zero -- no 0x55AA anywhere */
    CHECK(fat32ro_mount(&vol, disk_read, NULL) == FAT32RO_ERROR_NOT_FAT32,
          "an all-zero sector 0 is rejected");
}

static void test_mount_rejects_bad_bpb_fields(void)
{
    fs_layout_t l = standard_layout(0);
    fat32ro_volume_t vol;

    reset_disk();
    l.sectors_per_cluster = 3; /* not a power of two -- invalid */
    format_disk(&l);

    CHECK(fat32ro_mount(&vol, disk_read, NULL) != FAT32RO_SUCCESS,
          "invalid sectors_per_cluster is rejected");
}

static void test_mount_reports_unsupported_for_non_fat32_partition(void)
{
    /* An MBR whose only partition is a real (non-FAT32) filesystem --
     * e.g. NTFS or exFAT -- is a genuinely different situation from a
     * blank/raw drive: the caller must not then go on to guess this
     * might be a raw v1/v2 movie image. */
    fat32ro_volume_t vol;
    uint8_t *mbr;

    reset_disk();
    mbr = g_disk[0];
    mbr[446 + 4] = 0x07; /* NTFS/exFAT type, not FAT32 */
    mbr[510] = 0x55; mbr[511] = 0xAA;

    CHECK(fat32ro_mount(&vol, disk_read, NULL) == FAT32RO_ERROR_UNSUPPORTED_FILESYSTEM,
          "an MBR with a real but non-FAT32 partition is reported as unsupported, not NOT_FAT32");
}

static void test_mount_rejects_empty_mbr_as_not_fat32(void)
{
    /* A valid boot signature but a completely empty partition table (all
     * four entries type 0x00) is what a genuinely blank/unformatted
     * drive (or a raw whole-device movie image, which also has no
     * partition table) looks like -- this is the one case that should
     * still get NOT_FAT32, inviting the caller to try raw detection. */
    fat32ro_volume_t vol;
    uint8_t *mbr;

    reset_disk();
    mbr = g_disk[0];
    mbr[510] = 0x55; mbr[511] = 0xAA;

    CHECK(fat32ro_mount(&vol, disk_read, NULL) == FAT32RO_ERROR_NOT_FAT32,
          "an empty partition table is treated as no filesystem at all");
}

static void test_mount_reports_unsupported_for_fat16_superfloppy(void)
{
    /* A directly-formatted (no MBR) FAT16 volume: same jump-instruction
     * boot sector shape as FAT32, but FATSz32 (offset 36) reads as 0
     * since FAT16 doesn't have that field, so parse_bpb() correctly
     * rejects it -- and because it's still boot-sector-shaped, mount()
     * must report this as a real (if unsupported) filesystem rather
     * than silently falling through to "no filesystem at all". */
    fat32ro_volume_t vol;
    uint8_t *s = g_disk[0];

    reset_disk();
    s[0] = 0xEB; s[1] = 0x3C; s[2] = 0x90;
    memcpy(s + 3, "MSDOS5.0", 8);
    put_u16le(s + 11, 512);
    s[13] = 4;               /* sectors per cluster */
    put_u16le(s + 14, 1);    /* reserved sectors */
    s[16] = 2;                /* num FATs */
    put_u16le(s + 22, 32);   /* FATSz16 -- the FAT16 field at this offset */
    s[510] = 0x55; s[511] = 0xAA;

    CHECK(fat32ro_mount(&vol, disk_read, NULL) == FAT32RO_ERROR_UNSUPPORTED_FILESYSTEM,
          "a FAT16 superfloppy boot sector is reported as unsupported, not NOT_FAT32");
}

static void test_mount_propagates_read_failure(void)
{
    fat32ro_volume_t vol;

    g_fail_calls = 0;
    CHECK(fat32ro_mount(&vol, disk_read_always_fails, NULL) == FAT32RO_ERROR_READ_FAILED,
          "a failing read_sectors is reported, not silently ignored");
    CHECK(g_fail_calls > 0, "read_sectors was actually invoked");
}

/* --- directory listing --------------------------------------------------- */

static void test_list_root_finds_files_and_skips_others(void)
{
    fs_layout_t l = standard_layout(0);
    fat32ro_volume_t vol;
    fat32ro_dirent_t entries[8];
    int count;

    reset_disk();
    format_disk(&l);

    write_dir_entry(&l, 2, 0, "MOVIE01 BIN", 0x20 /* archive */, 10, 12345);
    write_dir_entry(&l, 2, 1, "SUBDIR     ", 0x10 /* directory */, 20, 0);
    write_dir_entry(&l, 2, 2, "VOLUME  LAB", 0x08 /* volume label */, 0, 0);
    write_dir_entry(&l, 2, 3, "LONGNAME~1 ", 0x0F /* LFN fragment marker */, 0, 0);
    {
        uint8_t *deleted = g_disk[cluster_lba(&l, 2)] + 4 * 32;
        deleted[0] = 0xE5;
    }
    write_dir_entry(&l, 2, 5, "MOVIE02 BIN", 0x20, 30, 999);

    CHECK(fat32ro_mount(&vol, disk_read, NULL) == FAT32RO_SUCCESS, "mount for listing test");

    count = fat32ro_list_root(&vol, entries, 8);
    CHECK(count == 2, "only the two real files are listed");
    if (count == 2) {
        CHECK(strcmp(entries[0].name, "MOVIE01.BIN") == 0, "first entry name reconstructed correctly");
        CHECK(entries[0].first_cluster == 10, "first entry cluster correct");
        CHECK(entries[0].file_size == 12345, "first entry size correct");
        CHECK(strcmp(entries[1].name, "MOVIE02.BIN") == 0, "second entry name reconstructed correctly");
        CHECK(entries[1].first_cluster == 30, "second entry cluster correct");
    }
}

static void test_list_root_stops_at_end_marker(void)
{
    fs_layout_t l = standard_layout(0);
    fat32ro_volume_t vol;
    fat32ro_dirent_t entries[8];
    int count;

    reset_disk();
    format_disk(&l);
    write_dir_entry(&l, 2, 0, "MOVIE01 BIN", 0x20, 10, 100);
    /* entry 1 left all-zero -> first byte 0x00 -> end of directory */
    write_dir_entry(&l, 2, 2, "MOVIE02 BIN", 0x20, 30, 100); /* must NOT be seen */

    CHECK(fat32ro_mount(&vol, disk_read, NULL) == FAT32RO_SUCCESS, "mount for end-marker test");
    count = fat32ro_list_root(&vol, entries, 8);
    CHECK(count == 1, "listing stops at the first 0x00 entry");
}

static void test_list_root_caps_at_max_entries_without_misreporting(void)
{
    fs_layout_t l = standard_layout(0);
    fat32ro_volume_t vol;
    fat32ro_dirent_t entries[2];
    int count;
    int i;

    reset_disk();
    format_disk(&l);
    for (i = 0; i < 5; ++i) {
        char name[12];
        sprintf(name, "MOVIE%02d BIN", i);
        write_dir_entry(&l, 2, i, name, 0x20, (uint32_t)(10 + i), 100);
    }

    CHECK(fat32ro_mount(&vol, disk_read, NULL) == FAT32RO_SUCCESS, "mount for cap test");
    count = fat32ro_list_root(&vol, entries, 2);
    CHECK(count == 2, "caller's array size caps the returned count");
}

/* --- extent mapping ------------------------------------------------------ */

static void test_extent_map_contiguous_file(void)
{
    fs_layout_t l = standard_layout(0);
    fat32ro_volume_t vol;
    fat32ro_extent_map_t map;
    uint32_t file_size = 3 * l.sectors_per_cluster * 512u; /* exactly 3 clusters */

    reset_disk();
    format_disk(&l);
    set_fat_entry(&l, 10, 11);
    set_fat_entry(&l, 11, 12);
    set_fat_entry(&l, 12, 0x0FFFFFFF);

    CHECK(fat32ro_mount(&vol, disk_read, NULL) == FAT32RO_SUCCESS, "mount for contiguous test");
    CHECK(fat32ro_build_extent_map(&vol, 10, file_size, &map) == FAT32RO_SUCCESS,
          "contiguous 3-cluster file maps successfully");
    CHECK(map.extent_count == 1, "a contiguous file collapses to exactly one extent");
    if (map.extent_count == 1) {
        CHECK(map.extents[0].lba == cluster_lba(&l, 10), "extent LBA matches cluster 10's LBA");
        CHECK(map.extents[0].sectors == 3 * l.sectors_per_cluster, "extent covers all 3 clusters");
    }
    CHECK(map.total_sectors == 3u * l.sectors_per_cluster, "total_sectors matches");
}

static void test_extent_map_fragmented_file(void)
{
    fs_layout_t l = standard_layout(0);
    fat32ro_volume_t vol;
    fat32ro_extent_map_t map;
    uint32_t file_size = 5 * l.sectors_per_cluster * 512u; /* 5 clusters, 3 runs: [10,11] [12] [50,51] */

    reset_disk();
    format_disk(&l);
    set_fat_entry(&l, 10, 11);
    set_fat_entry(&l, 11, 12);
    set_fat_entry(&l, 12, 50); /* non-consecutive jump: new extent */
    set_fat_entry(&l, 50, 51);
    set_fat_entry(&l, 51, 0x0FFFFFFF);

    CHECK(fat32ro_mount(&vol, disk_read, NULL) == FAT32RO_SUCCESS, "mount for fragmented test");
    CHECK(fat32ro_build_extent_map(&vol, 10, file_size, &map) == FAT32RO_SUCCESS,
          "fragmented file still maps successfully");
    CHECK(map.extent_count == 2, "two contiguous runs coalesce into two extents "
          "(10-11 and 12 are one run since 12 doesn't start a *new* run boundary until "
          "the jump to 50)");
    if (map.extent_count == 2) {
        CHECK(map.extents[0].lba == cluster_lba(&l, 10), "first extent starts at cluster 10");
        CHECK(map.extents[0].sectors == 3 * l.sectors_per_cluster, "first extent covers clusters 10-12");
        CHECK(map.extents[1].lba == cluster_lba(&l, 50), "second extent starts at cluster 50");
        CHECK(map.extents[1].sectors == 2 * l.sectors_per_cluster, "second extent covers clusters 50-51");
    }
    CHECK(map.total_sectors == 5u * l.sectors_per_cluster, "total_sectors still matches file size");
}

static void test_extent_map_rounds_up_to_cluster_but_not_beyond(void)
{
    fs_layout_t l = standard_layout(0);
    fat32ro_volume_t vol;
    fat32ro_extent_map_t map;
    /* File needs only 3 sectors, well within cluster 10's 8 sectors. */
    uint32_t file_size = 3u * 512u;

    reset_disk();
    format_disk(&l);
    set_fat_entry(&l, 10, 0x0FFFFFFF); /* single-cluster file */

    CHECK(fat32ro_mount(&vol, disk_read, NULL) == FAT32RO_SUCCESS, "mount for rounding test");
    CHECK(fat32ro_build_extent_map(&vol, 10, file_size, &map) == FAT32RO_SUCCESS,
          "small file within one cluster maps successfully");
    CHECK(map.extent_count == 1, "one cluster -> one extent");
    CHECK(map.total_sectors == l.sectors_per_cluster,
          "map covers the whole allocated cluster, not just the requested 3 sectors "
          "(trailing bytes are the file's own allocated padding, not a bug)");
}

static void test_extent_map_zero_length_file(void)
{
    fs_layout_t l = standard_layout(0);
    fat32ro_volume_t vol;
    fat32ro_extent_map_t map;

    reset_disk();
    format_disk(&l);

    CHECK(fat32ro_mount(&vol, disk_read, NULL) == FAT32RO_SUCCESS, "mount for zero-length test");
    /* first_cluster == 0 is legitimate for a genuinely empty file. */
    CHECK(fat32ro_build_extent_map(&vol, 0, 0, &map) == FAT32RO_SUCCESS,
          "zero-length file with first_cluster 0 is not an error");
    CHECK(map.extent_count == 0 && map.total_sectors == 0, "zero-length file maps to an empty extent list");
}

static void test_extent_map_rejects_invalid_first_cluster(void)
{
    fs_layout_t l = standard_layout(0);
    fat32ro_volume_t vol;
    fat32ro_extent_map_t map;

    reset_disk();
    format_disk(&l);

    CHECK(fat32ro_mount(&vol, disk_read, NULL) == FAT32RO_SUCCESS, "mount for bad-cluster test");
    CHECK(fat32ro_build_extent_map(&vol, 0, 4096, &map) == FAT32RO_ERROR_CLUSTER_CHAIN,
          "first_cluster 0 with nonzero size is rejected");
    CHECK(fat32ro_build_extent_map(&vol, 1, 4096, &map) == FAT32RO_ERROR_CLUSTER_CHAIN,
          "first_cluster 1 (reserved) is rejected");
}

static void test_extent_map_rejects_premature_end_of_chain(void)
{
    fs_layout_t l = standard_layout(0);
    fat32ro_volume_t vol;
    fat32ro_extent_map_t map;
    /* Declares needing 3 clusters' worth, but the chain ends after 1. */
    uint32_t file_size = 3 * l.sectors_per_cluster * 512u;

    reset_disk();
    format_disk(&l);
    set_fat_entry(&l, 10, 0x0FFFFFFF); /* ends immediately */

    CHECK(fat32ro_mount(&vol, disk_read, NULL) == FAT32RO_SUCCESS, "mount for short-chain test");
    CHECK(fat32ro_build_extent_map(&vol, 10, file_size, &map) == FAT32RO_ERROR_CLUSTER_CHAIN,
          "a chain shorter than the declared file size is rejected, not read past");
}

static void test_extent_map_rejects_infinite_loop(void)
{
    fs_layout_t l = standard_layout(0);
    fat32ro_volume_t vol;
    fat32ro_extent_map_t map;
    uint32_t file_size = 100u * l.sectors_per_cluster * 512u; /* more than the loop can ever satisfy */

    reset_disk();
    format_disk(&l);
    /* 10 -> 11 -> 10 -> 11 -> ... never terminates and never reaches
     * the requested size. Must be caught by loop-step bound, not hang. */
    set_fat_entry(&l, 10, 11);
    set_fat_entry(&l, 11, 10);

    CHECK(fat32ro_mount(&vol, disk_read, NULL) == FAT32RO_SUCCESS, "mount for loop test");
    CHECK(fat32ro_build_extent_map(&vol, 10, file_size, &map) == FAT32RO_ERROR_CLUSTER_CHAIN,
          "a looping cluster chain terminates with an error instead of hanging");
}

static void test_extent_map_too_fragmented_is_rejected_cleanly(void)
{
    /* sectors_per_cluster=1 keeps each "run" to a single cluster, and
     * every cluster points two ahead (10->12->14->...), guaranteeing no
     * two consecutive clusters ever coalesce -- FAT32RO_MAX_EXTENTS+10
     * single-cluster runs must exceed the bound. */
    fs_layout_t l;
    fat32ro_volume_t vol;
    fat32ro_extent_map_t map;
    const uint32_t num_clusters = FAT32RO_MAX_EXTENTS + 10;
    uint32_t i;
    uint32_t file_size;

    reset_disk();
    l = standard_layout(0);
    l.sectors_per_cluster = 1;
    l.fat_size_sectors = 16; /* 16*512/4 = 2048 entries -- enough headroom */
    /* Clusters used go up to 10 + (num_clusters-1)*2 (every other cluster
     * number, starting at 10) -- total_clusters must comfortably exceed
     * that highest cluster number, not just num_clusters itself. */
    l.total_sectors = 1 + 16 + 1 * (10 + num_clusters * 2 + 10);
    format_disk(&l);

    for (i = 0; i < num_clusters - 1; ++i) {
        set_fat_entry(&l, 10 + i * 2, 10 + (i + 1) * 2);
    }
    set_fat_entry(&l, 10 + (num_clusters - 1) * 2, 0x0FFFFFFF);

    file_size = num_clusters * 512u;

    CHECK(fat32ro_mount(&vol, disk_read, NULL) == FAT32RO_SUCCESS, "mount for over-fragmented test");
    CHECK(fat32ro_build_extent_map(&vol, 10, file_size, &map) == FAT32RO_ERROR_TOO_FRAGMENTED,
          "a file needing more than FAT32RO_MAX_EXTENTS runs is rejected, not overflowed");
}

/* --- extent lookup -------------------------------------------------------- */

static void test_extent_lookup_across_boundaries(void)
{
    fat32ro_extent_map_t map;
    uint32_t lba, run;

    memset(&map, 0, sizeof(map));
    map.extents[0].lba = 1000; map.extents[0].sectors = 15; /* covers offsets 0..14 */
    map.extents[1].lba = 5000; map.extents[1].sectors = 30; /* covers offsets 15..44 */
    map.extent_count = 2;
    map.total_sectors = 45;

    CHECK(fat32ro_extent_lookup(&map, 0, &lba, &run) && lba == 1000 && run == 15,
          "offset 0 resolves to the start of the first extent with its full run length");
    CHECK(fat32ro_extent_lookup(&map, 14, &lba, &run) && lba == 1014 && run == 1,
          "the last sector of the first extent reports run length 1 (boundary next)");
    CHECK(fat32ro_extent_lookup(&map, 15, &lba, &run) && lba == 5000 && run == 30,
          "offset 15 correctly crosses into the second extent");
    CHECK(fat32ro_extent_lookup(&map, 44, &lba, &run) && lba == 5029 && run == 1,
          "the last valid offset resolves correctly");
    CHECK(!fat32ro_extent_lookup(&map, 45, &lba, &run), "one past total_sectors is out of range");
    CHECK(!fat32ro_extent_lookup(&map, 1000000, &lba, &run), "a wildly out-of-range offset is rejected");
}

int main(void)
{
    test_mount_superfloppy();
    test_mount_rejects_swapped_boot_signature();
    test_mount_realistic_small_fat32_drive();
    test_mount_mbr_partition();
    test_mount_mbr_partition_with_stale_type_byte();
    test_mount_rejects_missing_signature();
    test_mount_rejects_bad_bpb_fields();
    test_mount_reports_unsupported_for_non_fat32_partition();
    test_mount_rejects_empty_mbr_as_not_fat32();
    test_mount_reports_unsupported_for_fat16_superfloppy();
    test_mount_propagates_read_failure();

    test_list_root_finds_files_and_skips_others();
    test_list_root_stops_at_end_marker();
    test_list_root_caps_at_max_entries_without_misreporting();

    test_extent_map_contiguous_file();
    test_extent_map_fragmented_file();
    test_extent_map_rounds_up_to_cluster_but_not_beyond();
    test_extent_map_zero_length_file();
    test_extent_map_rejects_invalid_first_cluster();
    test_extent_map_rejects_premature_end_of_chain();
    test_extent_map_rejects_infinite_loop();
    test_extent_map_too_fragmented_is_rejected_cleanly();

    test_extent_lookup_across_boundaries();

    if (g_failures == 0) {
        printf("All fat32ro tests passed.\n");
        return 0;
    }
    printf("%d test(s) failed.\n", g_failures);
    return 1;
}
