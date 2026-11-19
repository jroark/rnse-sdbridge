/* The bridge's side of src/aalink.h: the phone's video messages made
 * into blocks in a ring in RAM, the blocks read out as sectors, and the
 * ring let go as the 192 says it has read them.
 *
 * One writer and one reader: on the RP2040 the session code writes
 * (aastream_begin, _add, _end, from the main loop) and the data side
 * reads (aastream_sector, aastream_consumed, from an interrupt of the
 * same core, which the main loop never interrupts).  A block is complete
 * before head passes it, and the writer touches only the block at head.
 *
 * Plain C with no SDK (sdbridge/tests/host.c). */
#ifndef AASTREAM_H
#define AASTREAM_H
#include <stdint.h>
#include "aalink.h"
#include "volume.h"   /* BRIDGE_RAM */

struct aastream {
    uint8_t (*ring)[512];
    uint32_t blocks;              /* in the ring */
    volatile uint32_t head;       /* the next block to complete; those below are ready */
    volatile uint32_t tail;       /* the oldest block kept */
    volatile uint32_t sent;       /* one past the last block that has gone out whole */
    uint32_t fill;                /* payload bytes in the block at head (0: none begun) */
    uint16_t flags;               /* for the block at head */
    int in_message;
    uint32_t key;                 /* first block of the last key frame, or AALINK_NONE */
    uint16_t state;               /* AALINK_PHONE, AALINK_VIDEO */
    uint16_t width, height;
    uint8_t config[AALINK_CONFIG_MAX];
    uint32_t config_len;
    int config_taking;            /* the message under way is a configuration */
    uint32_t messages, key_asked, paused;
};

void aastream_init(struct aastream *s, void *ring, uint32_t blocks);
/* Payload bytes that can be added before the ring is full. */
uint32_t aastream_room(const struct aastream *s);
/* A message: type AALINK_CONFIG or AALINK_PICTURE, key when it is a key
 * frame (aastream_is_key says).  aastream_add takes no more than
 * aastream_room allows: the bytes it took.  aastream_end publishes the
 * last block and returns its seq. */
void aastream_begin(struct aastream *s, int type, int key);
uint32_t aastream_add(struct aastream *s, const uint8_t *p, uint32_t n);
uint32_t aastream_end(struct aastream *s);
/* Whether H.264 bytes (start codes and all) have a slice of a key frame
 * (an IDR, NAL type 5) among their first NAL units. */
int aastream_is_key(const uint8_t *p, uint32_t n);

/* Sector `index` of AALINK.BIN (control sectors, then the window) as
 * 128 words, each its four bytes most significant first.  Reading
 * AALINK_CTL_KEY counts a key frame asked for. */
void aastream_sector(struct aastream *s, uint32_t index, uint32_t *words);
/* The block a window sector holds: the seq of that sector nearest the
 * blocks kept. */
uint32_t aastream_seq(const struct aastream *s, uint32_t window_index);
/* The 192 has started a read at window sector `window_index`: the
 * blocks before it are let go. */
void aastream_consumed(struct aastream *s, uint32_t window_index);
/* Block seq has gone out whole (for the acknowledgements). */
void aastream_went_out(struct aastream *s, uint32_t seq);

#endif
