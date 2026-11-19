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
    uint32_t fill;                /* payload bytes in the block at head */
    uint16_t flags;               /* for the block at head */
    int open;                     /* the channel whose block is at head, -1 none */
    /* Each channel's message under way (video's and the sound's are
     * interleaved: aalink.h). */
#define AASTREAM_CHANNELS 4       /* 0 video; 1-3 sound: media, speech, system */
    struct aastream_channel {
        int in_message;
        uint8_t type;
        uint16_t pending;         /* START (and KEY) for its next block */
    } ch[AASTREAM_CHANNELS];
    uint32_t key;                 /* first block of the last key frame, or AALINK_NONE */
    uint16_t state;               /* AALINK_PHONE, AALINK_VIDEO */
    uint16_t width, height;
    uint8_t config[AALINK_CONFIG_MAX];
    uint32_t config_len;
    int config_taking;            /* the message under way is a configuration */
    uint32_t messages, key_asked, paused;
    /* Input from the 192 (aalink.h, "Input"), for the session to take. */
#define AASTREAM_INPUTS 16
    uint16_t input[AASTREAM_INPUTS];
    volatile uint32_t input_in, input_out;
    int input_seq;                /* the last event's code, -1 none */
    uint32_t inputs;
};

void aastream_init(struct aastream *s, void *ring, uint32_t blocks);
/* Payload bytes that can be added before the ring is full. */
uint32_t aastream_room(const struct aastream *s);
/* A message on channel ch (0 video, 1-3 sound): type AALINK_CONFIG or
 * AALINK_PICTURE (channel 0) or one of the sound's, key when it is a key
 * frame (aastream_is_key says).  aastream_add takes no more than
 * aastream_room allows: the bytes it took.  aastream_end publishes the
 * message's last block and returns its seq.  The channels' messages may
 * be under way at the same time: their blocks are interleaved. */
void aastream_begin(struct aastream *s, int ch, int type, int key);
uint32_t aastream_add(struct aastream *s, int ch, const uint8_t *p, uint32_t n);
uint32_t aastream_end(struct aastream *s, int ch);
/* Whether H.264 bytes (start codes and all) have a slice of a key frame
 * (an IDR, NAL type 5) among their first NAL units. */
int aastream_is_key(const uint8_t *p, uint32_t n);

/* Sector `index` of AALINK.BIN (control sectors, the window, the input
 * sectors) as 128 words, each its four bytes most significant first.
 * Nothing else: a sector is also made ahead of being asked for (the
 * next of a multiple read), so making one says nothing. */
void aastream_sector(struct aastream *s, uint32_t index, uint32_t *words);
/* The 192 has started a read at sector `index` of AALINK.BIN: an input
 * sector's event, or AALINK_CTL_KEY's key frame asked for.  (Taken when
 * the sector was made, the first input sectors, made ahead after a read
 * that ended at the window's end, were a HOME let go and pressed, and
 * Android Auto went to its launcher every two minutes or so: a unit,
 * 2026-11-18.) */
void aastream_request(struct aastream *s, uint32_t index);
/* Whether sector `index` of AALINK.BIN is one of the window's (not a
 * control sector before it nor an input sector after it), and which. */
int aastream_in_window(uint32_t index, uint32_t *window_index);
/* The block a window sector holds: the seq of that sector nearest the
 * blocks kept. */
uint32_t aastream_seq(const struct aastream *s, uint32_t window_index);
/* The 192 has started a read at window sector `window_index`: the
 * blocks before it are let go. */
void aastream_consumed(struct aastream *s, uint32_t window_index);
/* An input event the 192 sent, for the session: 1 with its code
 * (aalink.h), 0 none. */
int aastream_input_take(struct aastream *s, uint16_t *code);
/* Block seq has gone out whole (for the acknowledgements). */
void aastream_went_out(struct aastream *s, uint32_t seq);

#endif
