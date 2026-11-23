#include <stdio.h>
#include <string.h>
#include "aa_feed.h"
#include "aalink.h"

static uint32_t ms(struct aa_feed *f)
{
    return (uint32_t)(f->ses.io.us(f->ses.io.ctx) / 1000u);
}

static int is_key(const uint8_t *p, uint32_t n)
{
    /* As aastream_is_key: a slice of an IDR picture among the first NAL
     * units. */
    uint32_t i;
    for (i = 0; i + 3 < n; i++)
        if (!p[i] && !p[i + 1] && p[i + 2] == 1 && (p[i + 3] & 0x1f) == 5)
            return 1;
    return 0;
}

/* A video frame of a media message, decrypted: into the stream. */
static void video_frame(struct aa_feed *f, uint8_t flags, const uint8_t *p, uint32_t n)
{
    const struct aa_sink *k = &f->sink;
    if (flags & AAP_FIRST) {
        uint16_t id = (uint16_t)(p[0] << 8 | p[1]);
        int type = id == AA_AV_MEDIA ? AALINK_CONFIG : AALINK_PICTURE;
        uint32_t skip = id == AA_AV_MEDIA ? 2 : 10;  /* the id, and the timestamp */
        if (n < skip)
            return;
        p += skip;
        n -= skip;
        k->begin(k->ctx, 0, type, type == AALINK_PICTURE && is_key(p, n < 64 ? n : 64));
        f->video_open = 1;
    }
    if (!f->video_open)
        return;
    {
        uint32_t took = k->add(k->ctx, 0, p, n);
        if (took < n)
            f->dropped_bytes += n - took;   /* should not happen: room was looked at first */
    }
    if (flags & AAP_LAST) {
        k->end(k->ctx, 0);
        f->video_open = 0;
        f->videos_in++;
        if (!f->streaming) {
            f->streaming = 1;
            printf("video arriving, %lu ms after authentication\n",
                (unsigned long)((f->ses.io.us(f->ses.io.ctx) - f->ses.t_auth) / 1000));
            if (f->event)
                f->event(f->event_ctx, AA_FEED_STREAMING);
        }
    }
}

/* A sound frame of a media message, decrypted: into the stream as its
 * channel's, or dropped, a message at a time, when the 192 has read
 * nothing for a second (no viewer: it would only fill the stream) or the
 * stream is nearly full (the pictures come first: the phone's bytes are
 * held back for them, never for sound). */
static void sound_frame(struct aa_feed *f, uint8_t channel, uint8_t flags, const uint8_t *p, uint32_t n)
{
    const struct aa_sink *k = &f->sink;
    int ch = channel - AA_CH_MEDIA_AUDIO + 1;
    if (ch < 1 || ch >= AA_FEED_CHANNELS)
        return;
    if (flags & AAP_FIRST) {
        uint16_t id = (uint16_t)(p[0] << 8 | p[1]);
        uint32_t skip = id == AA_AV_MEDIA ? 2 : 10;
        if (n < skip)
            return;
        p += skip;
        n -= skip;
        f->sound_open[ch] = k->reading(k->ctx) && k->room(k->ctx) >= AA_FEED_FRAME_ROOM + 8192;
        if (f->sound_open[ch])
            k->begin(k->ctx, ch, AALINK_SOUND_MEDIA + ch - 1, 0);
        else
            f->sound_dropped++;
    }
    if (!f->sound_open[ch])
        return;
    if (k->add(k->ctx, ch, p, n) < n)
        f->dropped_bytes += n;
    if (flags & AAP_LAST) {
        k->end(k->ctx, ch);
        f->sound_open[ch] = 0;
    }
}

static void on_media(void *ctx, uint8_t channel, uint8_t flags, const uint8_t *p, uint32_t n)
{
    struct aa_feed *f = ctx;
    if ((flags & AAP_FIRST) && n < 2)
        return;
    if (channel == AA_CH_VIDEO)
        video_frame(f, flags, p, n);
    else
        sound_frame(f, channel, flags, p, n);
}

/* Messages under way ended: their blocks are in the stream already. */
static void end_open(struct aa_feed *f)
{
    int ch;
    if (f->video_open)
        f->sink.end(f->sink.ctx, 0);
    f->video_open = 0;
    for (ch = 1; ch < AA_FEED_CHANNELS; ch++) {
        if (f->sound_open[ch])
            f->sink.end(f->sink.ctx, ch);
        f->sound_open[ch] = 0;
    }
}

static void on_state(void *ctx, int state)
{
    struct aa_feed *f = ctx;
    if (state == AA_S_AUTHED) {
        f->videos_in = f->acked = 0;
        f->backlog = 0;
        f->ack_since = 0;
        f->streaming = 0;
        f->sink.phone(f->sink.ctx, 1, AA_HU_WIDTH, AA_HU_HEIGHT);
        f->key_asked_seen = f->sink.key_asked(f->sink.ctx);
    }
    if (f->event)
        f->event(f->event_ctx, state);
}

static int feed_send(void *ctx, const uint8_t *p, uint32_t n)
{
    struct aa_feed *f = ctx;
    return f->send(f->ctx, p, n);
}

static uint64_t feed_us(void *ctx)
{
    struct aa_feed *f = ctx;
    return f->us(f->ctx);
}

void aa_feed_init(struct aa_feed *f, int (*send)(void *ctx, const uint8_t *p, uint32_t n), uint64_t (*us)(void *ctx),
                  void *ctx, const struct aa_session_cert *cert, int max_unacked, const struct aa_sink *sink)
{
    struct aa_session_io io;
    void (*event)(void *, int) = f->event;
    void *event_ctx = f->event_ctx;
    /* All but the session, whose buffers aa_session_init need not clear. */
    memset(f, 0, offsetof(struct aa_feed, ses));
    f->event = event;
    f->event_ctx = event_ctx;
    f->sink = *sink;
    f->send = send;
    f->us = us;
    f->ctx = ctx;
    io.ctx = f;
    io.send = feed_send;
    io.us = feed_us;
    io.media = on_media;
    io.message = NULL;
    io.state = on_state;
    aa_session_init(&f->ses, &io, cert, max_unacked, 1);
}

int aa_feed_idle(const struct aa_feed *f)
{
    return !f->pend;
}

void aa_feed_give(struct aa_feed *f, const uint8_t *p, uint32_t n)
{
    f->pend = p;
    f->pend_len = n;
    f->pend_at = 0;
}

/* The phone's bytes: frames taken while the stream has room for one. */
static void phone_bytes(struct aa_feed *f)
{
    while (f->ses.state != AA_S_FAILED && f->ses.state != AA_S_IDLE) {
        if (f->sink.room(f->sink.ctx) < AA_FEED_FRAME_ROOM) {
            if (!f->paused) {
                f->paused = 1;
                f->pauses++;
            }
            return;
        }
        f->paused = 0;
        if (aa_session_step(&f->ses))
            continue;
        if (!f->pend)
            return;
        if (f->pend_at >= f->pend_len) {
            f->pend = NULL;
            return;
        }
        {
            uint32_t k = f->pend_len - f->pend_at < AAP_FEED_MAX ? f->pend_len - f->pend_at : AAP_FEED_MAX;
            if (aa_session_feed(&f->ses, f->pend + f->pend_at, k))
                return;
            f->pend_at += k;
        }
    }
    f->pend = NULL;
}

/* Acknowledgements for what the 192 has read, its keys and knob, and a
 * key frame when it asked for one. */
static void for_the_192(struct aa_feed *f)
{
    static const uint32_t key_codes[AALINK_KEYS] = AALINK_KEY_CODES;
    const struct aa_sink *k = &f->sink;
    int up = f->ses.state == AA_S_AUTHED;
    uint16_t code;
    uint32_t out;
    /* With no session, the 192's input is dropped. */
    while (k->input_take(k->ctx, &code)) {
        uint64_t us = f->ses.io.us(f->ses.io.ctx);
        if (AALINK_IN_KIND(code) == AALINK_IN_BACKLOG) {
            f->backlog = code & 0xff;
            continue;
        }
        if (!up)
            continue;
        if (AALINK_IN_KIND(code) == AALINK_IN_KNOB)
            aa_hu_rotary(&f->ses.hu, us, (int8_t)(code & 0xff));
        else if (AALINK_IN_KIND(code) == 0 && ((code & 0xff) >> 1) < AALINK_KEYS)
            aa_hu_key(&f->ses.hu, us, key_codes[(code & 0xff) >> 1], code & 1);
        aa_session_flush(&f->ses);
    }
    if (!up)
        return;
    /* A picture is acknowledged once its last block has gone to the 192,
     * at once while the 192 is less than two pictures behind with its
     * decoding, else when it catches up or ACK_HOLD_MS later, whichever
     * comes first.  The phone waits for the acknowledgement up to some
     * 300 ms and then drops a frame of its own ("Waiting for ack
     * timeout, video frame dropped" in a Pixel's log, 2026-11-18): held
     * within that, it sends no faster than the 192 decodes, 4 a second
     * at the least. */
    out = k->videos_out(k->ctx);
    if (out > f->videos_in)
        out = f->videos_in;
    if ((int32_t)(out - f->acked) > 0 && !f->ack_since)
        f->ack_since = ms(f) | 1;
    while ((int32_t)(out - f->acked) > 0 && f->ses.state == AA_S_AUTHED &&
           (f->backlog < 2 || ms(f) - f->ack_since >= ACK_HOLD_MS)) {
        f->ack_since = 0;
        aa_hu_video_ack(&f->ses.hu);
        f->acked++;
        /* Each sent at once: the session's queue holds only a few. */
        aa_session_flush(&f->ses);
        if ((int32_t)(out - f->acked) > 0)
            f->ack_since = ms(f) | 1;
    }
    {
        uint32_t asked = k->key_asked(k->ctx);
        if (asked != f->key_asked_seen) {
            f->key_asked_seen = asked;
            aa_hu_ask_key_frame(&f->ses.hu);
        }
    }
    aa_session_flush(&f->ses);
}

void aa_feed_run(struct aa_feed *f)
{
    phone_bytes(f);
    for_the_192(f);
}

int aa_feed_open(struct aa_feed *f)
{
    f->pend = NULL;
    f->paused = 0;
    return aa_session_open(&f->ses);
}

void aa_feed_close(struct aa_feed *f)
{
    int was = f->ses.state;
    end_open(f);
    f->pend = NULL;
    f->paused = 0;
    f->streaming = 0;
    f->videos_in = f->acked = 0;
    aa_session_close(&f->ses);
    if (was != AA_S_IDLE)
        f->sink.phone(f->sink.ctx, 0, 0, 0);
}
