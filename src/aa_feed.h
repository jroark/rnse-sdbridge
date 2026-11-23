/* The phone's side of the bridge, between the phone's bytes and the
 * stream of blocks the 192 reads (aaviewer/aalink.h): the session
 * (src/aa_session.h), its video and sound messages put into the stream
 * a frame at a time, the phone's bytes held back while the stream has no
 * room, each picture acknowledged once the 192 has it, and the 192's
 * keys and knob sent to the phone.
 *
 * The stream is reached through a sink, so that this is the same code
 * where the stream is in the same chip (SDLINK.UF2: src/aa_streamsink.c
 * on src/aastream.c) and where it is at the other end of a wire (the
 * ESP32-S3 with the phone, the RP2040 in the slot).
 *
 * Plain C with no SDK (tests/session_host.c). */
#ifndef AA_FEED_H
#define AA_FEED_H
#include "aa_session.h"
#include "aa_sink.h"

/* A picture's acknowledgement is held while the 192 is two or more
 * pictures behind with its decoding, this long at most (the phone waits
 * some 300 ms and then drops a frame of its own). */
#ifndef ACK_HOLD_MS
#define ACK_HOLD_MS 250
#endif
/* Room for a frame's payload, decrypted: below that, no frame is taken
 * from the phone's bytes until the 192 has read. */
#define AA_FEED_FRAME_ROOM (16384 + 64)

struct aa_feed {
    struct aa_sink sink;
    /* The link and the clock, with their context. */
    int (*send)(void *ctx, const uint8_t *p, uint32_t n);
    uint64_t (*us)(void *ctx);
    void *ctx;
    /* Called with AA_S_... as the session's state changes, and with
     * AA_FEED_STREAMING when the first video message is whole.  May be
     * null. */
    void (*event)(void *ctx, int state);
    void *event_ctx;
#define AA_FEED_STREAMING 100
    int streaming;
    int video_open, sound_open[AA_FEED_CHANNELS];
    uint32_t videos_in;         /* channel 0's messages ended, this session */
    uint32_t acked;             /* of them, acknowledged */
    uint32_t backlog;           /* the 192's pictures read and not decoded, as it last said */
    uint32_t ack_since;         /* when the oldest acknowledgement became due (ms | 1; 0 none) */
    uint32_t key_asked_seen;
    /* The phone's bytes not yet made into frames. */
    const uint8_t *pend;
    uint32_t pend_len, pend_at;
    int paused;
    uint32_t pauses, dropped_bytes, sound_dropped;
    struct aa_session ses;
};

/* send, us: the link and a clock, as aa_session_io's (ctx theirs). */
void aa_feed_init(struct aa_feed *f, int (*send)(void *ctx, const uint8_t *p, uint32_t n), uint64_t (*us)(void *ctx),
                  void *ctx, const struct aa_session_cert *cert, int max_unacked, const struct aa_sink *sink);
/* The link is up: 0, or -1. */
int aa_feed_open(struct aa_feed *f);
/* The link is gone: messages under way are ended, the sink told. */
void aa_feed_close(struct aa_feed *f);

/* 1 when the bytes last given have all been taken (or none were): the
 * next may be given. */
int aa_feed_idle(const struct aa_feed *f);
/* Bytes from the phone, which stay the caller's and unchanged until
 * aa_feed_idle. */
void aa_feed_give(struct aa_feed *f, const uint8_t *p, uint32_t n);
/* As often as the caller's loop turns: frames taken while the stream
 * has room, input and acknowledgements sent. */
void aa_feed_run(struct aa_feed *f);

#endif
