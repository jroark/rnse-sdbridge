#include "aastream.h"

BRIDGE_RAM static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

BRIDGE_RAM static void put16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

BRIDGE_RAM static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

void aastream_init(struct aastream *s, void *ring, uint32_t blocks)
{
    uint32_t i;
    uint8_t *b = ring;
    for (i = 0; i < blocks * 512; i++)
        b[i] = 0;
    s->ring = ring;
    s->blocks = blocks;
    s->head = s->tail = s->sent = 0;
    s->fill = 0;
    s->flags = 0;
    s->in_message = 0;
    s->key = AALINK_NONE;
    s->state = 0;
    s->width = s->height = 0;
    s->config_len = 0;
    s->config_taking = 0;
    s->messages = s->key_asked = s->paused = 0;
}

uint32_t aastream_room(const struct aastream *s)
{
    uint32_t used = s->head - s->tail, free_blocks;
    if (used >= s->blocks)
        return 0;
    free_blocks = s->blocks - used;
    /* The block at head is one of them; begun, part of it is used. */
    return free_blocks * AALINK_PAYLOAD - s->fill;
}

/* The block at head finished: its header, and head past it. */
static void publish(struct aastream *s, int last)
{
    uint8_t *b = s->ring[s->head % s->blocks];
    uint32_t sum = 0, i;
    uint16_t flags = (uint16_t)(s->flags | AALINK_READY | (last ? AALINK_END : 0));
    for (i = s->fill; i < AALINK_PAYLOAD; i++)
        b[AALINK_HEADER + i] = 0;
    for (i = 0; i < AALINK_PAYLOAD; i += 4)
        sum += get32(b + AALINK_HEADER + i);
    put32(b, AALINK_BLOCK_MAGIC);
    put32(b + 4, s->head);
    put32(b + 8, 0);                      /* head, filled in as the block is read */
    put16(b + 12, flags);
    put16(b + 14, s->fill);
    put32(b + 16, sum);
    s->flags = 0;
    s->fill = 0;
    /* The block whole before head says so (one core: the compiler's
     * order is enough). */
    __asm__ volatile("" ::: "memory");
    s->head++;
}

void aastream_begin(struct aastream *s, int type, int key)
{
    if (s->in_message)
        (void)aastream_end(s);
    s->in_message = 1;
    s->fill = 0;
    s->flags = (uint16_t)(AALINK_START | (key ? AALINK_KEY : 0) | (type & 15) << 8);
    s->config_taking = type == AALINK_CONFIG;
    if (s->config_taking)
        s->config_len = 0;
    if (key)
        s->key = s->head;
    if (type == AALINK_PICTURE)
        s->messages++;
    s->state |= AALINK_VIDEO;
}

uint32_t aastream_add(struct aastream *s, const uint8_t *p, uint32_t n)
{
    uint32_t taken = 0;
    if (s->config_taking)
        for (; s->config_len < AALINK_CONFIG_MAX && taken < n; taken++)
            s->config[s->config_len++] = p[taken];
    taken = 0;
    while (taken < n) {
        uint32_t k;
        uint8_t *b;
        if (s->fill == AALINK_PAYLOAD) {
            /* Full, and more to come: it goes, unless the ring has no
             * room for the next. */
            if (s->head + 1 - s->tail >= s->blocks)
                break;
            publish(s, 0);
        }
        if (s->head - s->tail >= s->blocks)
            break;
        b = s->ring[s->head % s->blocks] + AALINK_HEADER;
        k = AALINK_PAYLOAD - s->fill;
        if (k > n - taken)
            k = n - taken;
        for (uint32_t i = 0; i < k; i++)
            b[s->fill + i] = p[taken + i];
        s->fill += k;
        taken += k;
    }
    return taken;
}

uint32_t aastream_end(struct aastream *s)
{
    uint32_t seq = s->head;
    if (!s->in_message)
        return seq - 1;
    s->in_message = 0;
    s->config_taking = 0;
    /* (A message with no bytes is a block too.) */
    if (s->head - s->tail >= s->blocks)
        return seq - 1; /* no room even for that: the caller looked first */
    publish(s, 1);
    return seq;
}

int aastream_is_key(const uint8_t *p, uint32_t n)
{
    uint32_t i;
    for (i = 0; i + 3 < n; i++) {
        if (p[i] || p[i + 1] || p[i + 2] != 1)
            continue;
        if ((p[i + 3] & 0x1f) == 5)
            return 1;
        i += 2;
    }
    return 0;
}

BRIDGE_RAM uint32_t aastream_seq(const struct aastream *s, uint32_t window_index)
{
    uint32_t tail = s->tail;
    uint32_t seq = tail - tail % AALINK_WINDOW + window_index % AALINK_WINDOW;
    /* The one of the numbers that share the sector nearest tail. */
    if ((int32_t)(seq - tail) < -(int32_t)(AALINK_WINDOW / 2))
        seq += AALINK_WINDOW;
    else if ((int32_t)(seq - tail) >= (int32_t)(AALINK_WINDOW / 2))
        seq -= AALINK_WINDOW;
    return seq;
}

BRIDGE_RAM void aastream_consumed(struct aastream *s, uint32_t window_index)
{
    uint32_t seq = aastream_seq(s, window_index), head = s->head;
    /* Forward only, and no further than what there is. */
    if ((int32_t)(seq - s->tail) <= 0)
        return;
    if ((int32_t)(seq - head) > 0)
        seq = head;
    s->tail = seq;
}

BRIDGE_RAM void aastream_went_out(struct aastream *s, uint32_t seq)
{
    if ((int32_t)(seq + 1 - s->sent) > 0)
        s->sent = seq + 1;
}

BRIDGE_RAM static void words_of(const uint8_t *b, uint32_t *w)
{
    uint32_t i;
    for (i = 0; i < 128; i++)
        w[i] = get32(b + 4 * i);
}

BRIDGE_RAM static void state_sector(struct aastream *s, uint32_t *w)
{
    uint8_t b[512];
    uint32_t i, n = s->config_len;
    for (i = 0; i < 512; i++)
        b[i] = 0;
    put32(b + AALINK_CTL_O_MAGIC, AALINK_CTL_MAGIC);
    put32(b + AALINK_CTL_O_VERSION, AALINK_VERSION);
    put32(b + AALINK_CTL_O_HEAD, s->head);
    put32(b + AALINK_CTL_O_TAIL, s->tail);
    /* The last key frame, while its blocks are kept. */
    put32(b + AALINK_CTL_O_KEY, s->key != AALINK_NONE && (int32_t)(s->key - s->tail) >= 0 ? s->key : AALINK_NONE);
    put16(b + AALINK_CTL_O_FLAGS, s->state);
    put16(b + AALINK_CTL_O_CONFIG_LEN, n);
    put16(b + AALINK_CTL_O_WIDTH, s->width);
    put16(b + AALINK_CTL_O_HEIGHT, s->height);
    put32(b + AALINK_CTL_O_MESSAGES, s->messages);
    put32(b + AALINK_CTL_O_KEY_ASKED, s->key_asked);
    put32(b + AALINK_CTL_O_PAUSED, s->paused);
    for (i = 0; i < n; i++)
        b[AALINK_CTL_CONFIG + i] = s->config[i];
    words_of(b, w);
}

BRIDGE_RAM void aastream_sector(struct aastream *s, uint32_t index, uint32_t *w)
{
    uint32_t i;
    if (index < AALINK_CTL_SECTORS) {
        if (index == AALINK_CTL_KEY)
            s->key_asked++;
        if (index == AALINK_CTL_STATE || index == AALINK_CTL_KEY) {
            state_sector(s, w);
            return;
        }
        for (i = 0; i < 128; i++)
            w[i] = 0;
        return;
    }
    {
        uint32_t seq = aastream_seq(s, index - AALINK_CTL_SECTORS), head = s->head;
        if ((int32_t)(seq - s->tail) >= 0 && (int32_t)(seq - head) < 0) {
            words_of(s->ring[seq % s->blocks], w);
            w[2] = head;
            return;
        }
        /* Not (or no longer) there: "not ready", and where head is. */
        for (i = 0; i < 128; i++)
            w[i] = 0;
        w[0] = AALINK_BLOCK_MAGIC;
        w[1] = seq;
        w[2] = head;
    }
}
