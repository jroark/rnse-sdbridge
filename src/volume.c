#include "volume.h"

/* The layout, in sectors from the card's start:
 *
 *   0           partition table, one partition (type 6, FAT16)
 *   32          its boot sector (one reserved sector)
 *   33-96       the first FAT, 64 sectors
 *   97-160      the second
 *   161-192     the top directory, 512 entries
 *   193 on      clusters of 8 sectors (4 KiB) from number 2; the file
 *               is clusters 2 to 2049 */
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
static void put8(uint32_t *w, uint32_t at, uint32_t v)
{
    uint32_t shift = 24 - 8 * (at & 3);
    w[at >> 2] = (w[at >> 2] & ~(0xffu << shift)) | (v & 0xff) << shift;
}

static void put16(uint32_t *w, uint32_t at, uint32_t v)
{
    put8(w, at, v);
    put8(w, at + 1, v >> 8);
}

static void put32(uint32_t *w, uint32_t at, uint32_t v)
{
    put16(w, at, v);
    put16(w, at + 2, v >> 16);
}

static void puts_(uint32_t *w, uint32_t at, const char *s, int n)
{
    int i;
    for (i = 0; i < n; i++)
        put8(w, at + (uint32_t)i, (uint8_t)s[i]);
}

static void mbr(uint32_t *w)
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

static void boot(uint32_t *w)
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

static uint32_t fat_entry(uint32_t e)
{
    if (e == 0)
        return 0xfff8;
    if (e == 1 || e == LAST_CLUSTER)
        return 0xffff;
    if (e >= 2 && e < LAST_CLUSTER)
        return e + 1;
    return 0;
}

static void fat(uint32_t *w, uint32_t k)
{
    uint32_t i;
    for (i = 0; i < 256; i++)
        put16(w, 2 * i, fat_entry(k * 256 + i));
}

static void root(uint32_t *w)
{
    puts_(w, 0, "LINKBENCH  ", 11);
    put8(w, 11, 0x08);                    /* the volume's label */
    puts_(w, 32, "LINKTESTBIN", 11);
    put8(w, 32 + 11, 0x01);               /* read only */
    put16(w, 32 + 22, 0);                 /* 00:00 */
    put16(w, 32 + 24, (46 << 9) | (11 << 5) | 16); /* 2026-11-16 */
    put16(w, 32 + 18, (46 << 9) | (11 << 5) | 16);
    put16(w, 32 + 16, (46 << 9) | (11 << 5) | 16);
    put16(w, 32 + 26, 2);
    put32(w, 32 + 28, VOLUME_FILE_BYTES);
}

void volume_sector(uint32_t n, uint32_t *w)
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
