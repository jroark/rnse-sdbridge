/* What the card holds, made up as it is read: 64 MiB, a partition table
 * and one FAT16 partition with LINKTEST.BIN (8 MiB in one run of
 * clusters, its words those of tools/linktest.py) in its top directory,
 * and LINKBENC.BIN when one has been sent.  Nothing else is stored. */
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

/* A program for the unit to run, LINKBENC.BIN, after LINKTEST.BIN: its
 * bytes as the Mac sent them over USB (main.c), and how many; none (and
 * no directory entry) while the count is 0.  The SD loader runs it, as
 * the one program on the card (LINKTEST.BIN is too big to be one), and
 * again at RETURN, read anew each time: a new build needs no card. */
#define VOLUME_PROGRAM_MAX 0x1c000u      /* the loader's limit */
extern uint8_t volume_program[VOLUME_PROGRAM_MAX];
extern volatile uint32_t volume_program_bytes;

/* Its first sector, for tests. */
uint32_t volume_program_sector(void);

#endif
