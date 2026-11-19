#include "volume.h"
#include "aalink.h"

/* The layout, in sectors from the card's start:
 *
 *   0           partition table, one partition (type 6, FAT16)
 *   32          its boot sector (one reserved sector)
 *   33-96       the first FAT, 64 sectors
 *   97-160      the second
 *   161-192     the top directory, 512 entries
 *   193 on      clusters of 8 sectors (4 KiB) from number 2; the file
 *               is clusters 2 to 2049, LINKBENC.BIN from 2050 (28 at
 *               most), AALINK.BIN from 2078 (8193 clusters) */
#define PART 32u
#define PART_SECTORS (VOLUME_SECTORS - PART)
#define PER_CLUSTER 8u
#define FAT_SECTORS 64u
#define ROOT_ENTRIES 512u
#define FAT1 (PART + 1)
#define FAT2 (FAT1 + FAT_SECTORS)
#define ROOT (FAT2 + FAT_SECTORS)
#define DATA (ROOT + ROOT_ENTRIES * 32 / 512)
#define FILE_CLUSTERS (VOLUME_FILE_BYTES / (PER_CLUSTER * 512))
#define LAST_CLUSTER (2 + FILE_CLUSTERS - 1)
#define PROG_FIRST (LAST_CLUSTER + 1)
#define PROG_SECTOR (DATA + FILE_CLUSTERS * PER_CLUSTER)
#define CLUSTER_BYTES (PER_CLUSTER * 512)
#define LINK_FIRST (PROG_FIRST + VOLUME_PROGRAM_MAX / CLUSTER_BYTES)
#define LINK_CLUSTERS ((AALINK_FILE_SECTORS + PER_CLUSTER - 1) / PER_CLUSTER)
#define LINK_SECTOR (DATA + (LINK_FIRST - 2) * PER_CLUSTER)

uint8_t volume_program[VOLUME_PROGRAM_ROOM];
volatile uint32_t volume_program_bytes;
void (*volume_link)(uint32_t index, uint32_t *words);

uint32_t volume_link_sector(void)
{
    return LINK_SECTOR;
}

BRIDGE_RAM static uint32_t program_clusters(void)
{
    return (volume_program_bytes + CLUSTER_BYTES - 1) / CLUSTER_BYTES;
}

uint32_t volume_program_sector(void)
{
    return PROG_SECTOR;
}

uint32_t volume_pattern(uint32_t j)
{
    uint32_t x = (j + 1) * 0x9e3779b1u;
    x ^= x >> 16;
    x *= 0x85ebca6bu;
    x ^= x >> 13;
    return x;
}

uint32_t volume_file_sector(void)
{
    return DATA;
}

/* Bytes into the big-endian words, at byte offset `at`. */
BRIDGE_RAM static void put8(uint32_t *w, uint32_t at, uint32_t v)
{
    uint32_t shift = 24 - 8 * (at & 3);
    w[at >> 2] = (w[at >> 2] & ~(0xffu << shift)) | (v & 0xff) << shift;
}

BRIDGE_RAM static void put16(uint32_t *w, uint32_t at, uint32_t v)
{
    put8(w, at, v);
    put8(w, at + 1, v >> 8);
}

BRIDGE_RAM static void put32(uint32_t *w, uint32_t at, uint32_t v)
{
    put16(w, at, v);
    put16(w, at + 2, v >> 16);
}

BRIDGE_RAM static void puts_(uint32_t *w, uint32_t at, const char *s, int n)
{
    int i;
    for (i = 0; i < n; i++)
        put8(w, at + (uint32_t)i, (uint8_t)s[i]);
}

BRIDGE_RAM static void mbr(uint32_t *w)
{
    put8(w, 446 + 0, 0x00);
    /* CHS fields as a formatter writes them for an LBA partition. */
    put8(w, 446 + 1, 0xfe);
    put8(w, 446 + 2, 0xff);
    put8(w, 446 + 3, 0xff);
    put8(w, 446 + 4, 0x06);
    put8(w, 446 + 5, 0xfe);
    put8(w, 446 + 6, 0xff);
    put8(w, 446 + 7, 0xff);
    put32(w, 446 + 8, PART);
    put32(w, 446 + 12, PART_SECTORS);
    put16(w, 510, 0xaa55);
}

BRIDGE_RAM static void boot(uint32_t *w)
{
    put8(w, 0, 0xeb);
    put8(w, 1, 0x3c);
    put8(w, 2, 0x90);
    puts_(w, 3, "RNSE    ", 8);
    put16(w, 11, 512);
    put8(w, 13, PER_CLUSTER);
    put16(w, 14, 1);                      /* reserved sectors */
    put8(w, 16, 2);                       /* FATs */
    put16(w, 17, ROOT_ENTRIES);
    put16(w, 19, 0);                      /* sectors: in the 32-bit field */
    put8(w, 21, 0xf8);
    put16(w, 22, FAT_SECTORS);
    put16(w, 24, 32);                     /* sectors a track */
    put16(w, 26, 64);                     /* heads */
    put32(w, 28, PART);                   /* hidden sectors */
    put32(w, 32, PART_SECTORS);
    put8(w, 36, 0x80);
    put8(w, 38, 0x29);
    put32(w, 39, 0x524e5345);
    puts_(w, 43, "LINKBENCH  ", 11);
    puts_(w, 54, "FAT16   ", 8);
    put16(w, 510, 0xaa55);
}

BRIDGE_RAM static uint32_t fat_entry(uint32_t e)
{
    if (e == 0)
        return 0xfff8;
    if (e == 1 || e == LAST_CLUSTER)
        return 0xffff;
    if (e >= 2 && e < LAST_CLUSTER)
        return e + 1;
    if (e >= PROG_FIRST && e < PROG_FIRST + program_clusters())
        return e + 1 == PROG_FIRST + program_clusters() ? 0xffff : e + 1;
    if (volume_link && e >= LINK_FIRST && e < LINK_FIRST + LINK_CLUSTERS)
        return e + 1 == LINK_FIRST + LINK_CLUSTERS ? 0xffff : e + 1;
    return 0;
}

BRIDGE_RAM static void fat(uint32_t *w, uint32_t k)
{
    uint32_t i;
    for (i = 0; i < 256; i++)
        put16(w, 2 * i, fat_entry(k * 256 + i));
}

/* A file's entry of the top directory, read only, dated. */
BRIDGE_RAM static void entry(uint32_t *w, uint32_t at, const char *name, uint32_t cluster, uint32_t size, uint32_t day)
{
    uint32_t date = (46 << 9) | (11 << 5) | day;          /* 2026-11 */
    puts_(w, at, name, 11);
    put8(w, at + 11, 0x01);
    put16(w, at + 22, 0);                                 /* 00:00 */
    put16(w, at + 24, date);
    put16(w, at + 18, date);
    put16(w, at + 16, date);
    put16(w, at + 26, cluster);
    put32(w, at + 28, size);
}

/* The entries one after another: a directory ends at the first empty
 * one (sdk/src/sd.c reads no further, as DOS does not). */
BRIDGE_RAM static void root(uint32_t *w)
{
    uint32_t at = 32;
    puts_(w, 0, "LINKBENCH  ", 11);
    put8(w, 11, 0x08);                    /* the volume's label */
    entry(w, at, "LINKTESTBIN", 2, VOLUME_FILE_BYTES, 16);
    at += 32;
    if (volume_program_bytes) {
        entry(w, at, "LINKBENCBIN", PROG_FIRST, volume_program_bytes, 18);
        at += 32;
    }
    if (volume_link)
        entry(w, at, AALINK_NAME, LINK_FIRST, AALINK_FILE_SECTORS * 512u, 18);
}

BRIDGE_RAM void volume_sector(uint32_t n, uint32_t *w)
{
    uint32_t i;
    if (n >= DATA) {
        uint32_t at = (n - DATA) * 128;
        if (at < VOLUME_FILE_BYTES / 4) {
            for (i = 0; i < 128; i++)
                w[i] = volume_pattern(at + i);
            return;
        }
    }
    if (volume_link && n >= LINK_SECTOR && n < LINK_SECTOR + AALINK_FILE_SECTORS) {
        volume_link(n - LINK_SECTOR, w);
        return;
    }
    if (n >= PROG_SECTOR && n < PROG_SECTOR + VOLUME_PROGRAM_ROOM / 512) {
        const uint8_t *b = volume_program + (n - PROG_SECTOR) * 512;
        for (i = 0; i < 128; i++)
            w[i] = (uint32_t)b[4 * i] << 24 | (uint32_t)b[4 * i + 1] << 16 | (uint32_t)b[4 * i + 2] << 8 | b[4 * i + 3];
        return;
    }
    for (i = 0; i < 128; i++)
        w[i] = 0;
    if (n == 0)
        mbr(w);
    else if (n == PART)
        boot(w);
    else if (n >= FAT1 && n < FAT2)
        fat(w, n - FAT1);
    else if (n >= FAT2 && n < ROOT)
        fat(w, n - FAT2);
    else if (n == ROOT)
        root(w);
}
