/* The stream in this chip's own RAM (src/aastream.h) as the sink of
 * src/aa_feed.h: SDLINK.UF2, where the session and the card are one
 * RP2040, and the RP2040 in the slot when the session is another
 * chip's, which then calls these as that chip's messages come.
 *
 * Called where aastream's writer is (the main loop). */
#ifndef AA_STREAMSINK_H
#define AA_STREAMSINK_H
#include "aa_sink.h"
#include "aastream.h"

struct aa_streamsink {
    struct aastream *s;
    /* Whether the 192 has read the window within the last second: the
     * card's side knows. */
    int (*reading)(void *ctx);
    void *ctx;
    /* The last block of each of channel 0's messages not yet gone out. */
#define AA_STREAMSINK_Q 16
    uint32_t q[AA_STREAMSINK_Q];
    unsigned q_in, q_out;
    uint32_t out;                 /* messages gone out, since the phone came */
};

void aa_streamsink_init(struct aa_streamsink *k, struct aastream *s, int (*reading)(void *ctx), void *ctx);
/* The sink, its ctx k. */
void aa_streamsink_sink(struct aa_streamsink *k, struct aa_sink *sink);

/* The sink's functions, for a caller that has them called from a wire. */
uint32_t aa_streamsink_room(void *k);
void aa_streamsink_begin(void *k, int ch, int type, int key);
uint32_t aa_streamsink_add(void *k, int ch, const uint8_t *p, uint32_t n);
void aa_streamsink_end(void *k, int ch);
uint32_t aa_streamsink_videos_out(void *k);
uint32_t aa_streamsink_key_asked(void *k);
int aa_streamsink_input_take(void *k, uint16_t *code);
int aa_streamsink_reading(void *k);
void aa_streamsink_phone(void *k, int up, int width, int height);

#endif
