#include "aa_streamsink.h"

void aa_streamsink_init(struct aa_streamsink *k, struct aastream *s, int (*reading)(void *ctx), void *ctx)
{
    k->s = s;
    k->reading = reading;
    k->ctx = ctx;
    k->q_in = k->q_out = 0;
    k->out = 0;
}

uint32_t aa_streamsink_room(void *ctx)
{
    struct aa_streamsink *k = ctx;
    return aastream_room(k->s);
}

void aa_streamsink_begin(void *ctx, int ch, int type, int key)
{
    struct aa_streamsink *k = ctx;
    aastream_begin(k->s, ch, type, key);
}

uint32_t aa_streamsink_add(void *ctx, int ch, const uint8_t *p, uint32_t n)
{
    struct aa_streamsink *k = ctx;
    return aastream_add(k->s, ch, p, n);
}

void aa_streamsink_end(void *ctx, int ch)
{
    struct aa_streamsink *k = ctx;
    uint32_t last;
    if (!k->s->ch[ch].in_message)
        return;
    last = aastream_end(k->s, ch);
    if (ch != 0)
        return;
    /* (More waiting than a phone may send ahead: the oldest counts as
     * gone out.) */
    if (k->q_in - k->q_out >= AA_STREAMSINK_Q) {
        k->q_out++;
        k->out++;
    }
    k->q[k->q_in++ % AA_STREAMSINK_Q] = last;
}

uint32_t aa_streamsink_videos_out(void *ctx)
{
    struct aa_streamsink *k = ctx;
    while (k->q_out != k->q_in && (int32_t)(k->s->sent - (k->q[k->q_out % AA_STREAMSINK_Q] + 1)) >= 0) {
        k->q_out++;
        k->out++;
    }
    return k->out;
}

uint32_t aa_streamsink_key_asked(void *ctx)
{
    struct aa_streamsink *k = ctx;
    return k->s->key_asked;
}

int aa_streamsink_input_take(void *ctx, uint16_t *code)
{
    struct aa_streamsink *k = ctx;
    return aastream_input_take(k->s, code);
}

int aa_streamsink_reading(void *ctx)
{
    struct aa_streamsink *k = ctx;
    return k->reading ? k->reading(k->ctx) : 1;
}

void aa_streamsink_phone(void *ctx, int up, int width, int height)
{
    struct aa_streamsink *k = ctx;
    int ch;
    /* Whatever was under way when the phone went is ended. */
    for (ch = 0; ch < AASTREAM_CHANNELS; ch++)
        if (k->s->ch[ch].in_message)
            (void)aastream_end(k->s, ch);
    k->q_in = k->q_out = 0;
    k->out = 0;
    if (up) {
        k->s->state |= AALINK_PHONE;
        k->s->width = (uint16_t)width;
        k->s->height = (uint16_t)height;
    } else {
        k->s->state &= (uint16_t)~(AALINK_PHONE | AALINK_VIDEO);
    }
}

void aa_streamsink_sink(struct aa_streamsink *k, struct aa_sink *sink)
{
    sink->ctx = k;
    sink->room = aa_streamsink_room;
    sink->begin = aa_streamsink_begin;
    sink->add = aa_streamsink_add;
    sink->end = aa_streamsink_end;
    sink->videos_out = aa_streamsink_videos_out;
    sink->key_asked = aa_streamsink_key_asked;
    sink->input_take = aa_streamsink_input_take;
    sink->reading = aa_streamsink_reading;
    sink->phone = aa_streamsink_phone;
}
