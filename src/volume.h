/* What the card holds, made up as it is read: 64 MiB, a partition table
 * and one FAT16 partition with LINKTEST.BIN (8 MiB in one run of
 * clusters, its words those of tools/linktest.py) in its top directory.
 * Nothing of it is stored. */
#ifndef VOLUME_H
#define VOLUME_H
#include <stdint.h>

#define VOLUME_SECTORS 131072u            /* 64 MiB */
#define VOLUME_FILE_BYTES (8u << 20)

/* Sector n as 128 words, each its four bytes most significant first (the
 * order they go out on the data line). */
void volume_sector(uint32_t n, uint32_t *words);

/* Word j of LINKTEST.BIN, as tools/linktest.py writes it. */
uint32_t volume_pattern(uint32_t j);

/* Where the file's first sector is, for tests. */
uint32_t volume_file_sector(void);

#endif
