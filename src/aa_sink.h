/* Where the phone's video and sound go, and where the 192's input and
 * its progress come from: the stream of blocks the 192 reads
 * (aaviewer/aalink.h), as src/aa_feed.c sees it.  In the same chip it
 * is src/aa_streamsink.c; from the ESP32-S3 it is a wire to the RP2040
 * in the slot. */
#ifndef AA_SINK_H
#define AA_SINK_H
#include <stdint.h>

/* The stream's channels: 0 video, 1-3 sound (media, speech, system), as
 * src/aastream.h has them. */
#define AA_FEED_CHANNELS 4

struct aa_sink {
    void *ctx;
    /* Payload bytes that can be added now. */
    uint32_t (*room)(void *ctx);
    /* A message on a channel: type as aalink.h's (AALINK_CONFIG, ...). */
    void (*begin)(void *ctx, int ch, int type, int key);
    /* The bytes taken (all of them, when room was looked at first). */
    uint32_t (*add)(void *ctx, int ch, const uint8_t *p, uint32_t n);
    void (*end)(void *ctx, int ch);
    /* How many of channel 0's messages, since `phone` last said up, have
     * gone to the 192 whole. */
    uint32_t (*videos_out)(void *ctx);
    /* A count that goes up each time the 192 asks for a key frame. */
    uint32_t (*key_asked)(void *ctx);
    /* An input event from the 192 (aalink.h, "Input"): 1 with its code. */
    int (*input_take)(void *ctx, uint16_t *code);
    /* Whether the 192 has read the stream within the last second. */
    int (*reading)(void *ctx);
    /* A session is up (with the video's size) or gone. */
    void (*phone)(void *ctx, int up, int width, int height);
};

#endif
