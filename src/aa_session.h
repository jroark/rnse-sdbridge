/* An Android Auto session from its first byte, whatever carries the
 * bytes: the version exchange, the TLS handshake (src/aa_tls.h),
 * "authentication complete", and from there the head unit (src/aa_hu.h)
 * with every frame decrypted on the way in and every answer encrypted on
 * the way out.  The caller owns the link (the phone's USB endpoints, a
 * TCP socket): it passes the phone's bytes in and is called with the
 * bytes to send.
 *
 *   aa_session_init once; aa_session_open when the link is up;
 *   then, for the bytes that come: aa_session_feed (a piece at a time),
 *   and aa_session_step until it says 0; aa_session_close when the link
 *   is gone.
 *
 * Plain C on src/aap.c, src/aa_tls.c and src/aa_hu.c, with no SDK: the
 * same on the RP2040, the ESP32-S3 and the host (tools/aa_tcp.c). */
#ifndef AA_SESSION_H
#define AA_SESSION_H
#include <stddef.h>
#include <stdint.h>
#include "aap.h"
#include "aa_tls.h"
#include "aa_hu.h"

enum {
    AA_S_IDLE,       /* no link */
    AA_S_VERSION,    /* the version request sent */
    AA_S_HANDSHAKE,  /* TLS under way */
    AA_S_AUTHED,     /* the session runs */
    AA_S_FAILED      /* until aa_session_close */
};

struct aa_session_io {
    void *ctx;
    /* Bytes for the phone, all of them: 0, or not 0 when the link failed. */
    int (*send)(void *ctx, const uint8_t *p, uint32_t n);
    /* A clock in microseconds. */
    uint64_t (*us)(void *ctx);
    /* A decrypted frame of a media message (video or sound, not
     * AAP_CONTROL) that the head unit has counted: p is the frame's
     * payload, on a first frame the message's id and timestamp first.
     * May be null. */
    void (*media)(void *ctx, uint8_t channel, uint8_t flags, const uint8_t *p, uint32_t n);
    /* Any other whole decrypted message, before the head unit answers
     * it.  May be null. */
    void (*message)(void *ctx, uint8_t channel, const uint8_t *m, uint32_t n);
    /* The state changed (AA_S_...).  May be null. */
    void (*state)(void *ctx, int state);
};

/* The head unit's certificate and key, as aa_tls_init takes them. */
struct aa_session_cert {
    const char *cert;
    size_t cert_len;
    const uint8_t *wire;
    size_t wire_len;
    const char *key;
    size_t key_len;
};

struct aa_session {
    int state;
    struct aa_session_io io;
    struct aa_session_cert cert;
    int max_unacked, defer_video_acks;
    int tls_up;
    struct aa_hu hu;            /* for aa_hu_key and the like; then aa_session_flush */
    uint64_t bytes_in;          /* from the phone */
    uint64_t decrypt_us;
    uint32_t frames_in;
    uint64_t t_auth;            /* io.us at "authentication complete" */
    struct aap_reader reader;
    struct aap_assembler msg;
    struct aa_tls tls;
    uint8_t tls_out[AA_TLS_OUT_BUF];
    uint8_t frame[AA_TLS_OUT_BUF + 8];
    uint8_t plain[AAP_FRAME_MAX];
};

void aa_session_init(struct aa_session *s, const struct aa_session_io *io, const struct aa_session_cert *cert,
                     int max_unacked, int defer_video_acks);
/* The link is up: the version request goes out.  0, or -1. */
int aa_session_open(struct aa_session *s);
/* The link is gone (or the session is given up). */
void aa_session_close(struct aa_session *s);

/* The phone's bytes, up to AAP_FEED_MAX of them (more only when every
 * frame has been taken out).  0, or -1 (the session has failed). */
int aa_session_feed(struct aa_session *s, const uint8_t *p, uint32_t n);
/* The next whole frame taken and acted on: 1, or 0 when there is none
 * (feed more) or the session has failed.  A caller with nowhere to put
 * a video frame just does not call it for a while. */
int aa_session_step(struct aa_session *s);
/* The head unit's queued messages sent (after aa_hu_key, aa_hu_rotary,
 * aa_hu_video_ack, aa_hu_ask_key_frame on s->hu). */
void aa_session_flush(struct aa_session *s);

#endif
