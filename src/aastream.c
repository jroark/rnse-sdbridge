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
    s->open = -1;
    for (i = 0; i < AASTREAM_CHANNELS; i++) {
        s->ch[i].in_message = 0;
        s->ch[i].pending = 0;
        s->ch[i].type = 0;
    }
    s->key = AALINK_NONE;
    s->state = 0;
    s->width = s->height = 0;
    s->config_len = 0;
    s->config_taking = 0;
    s->messages = s->key_asked = s->paused = 0;
    s->input_in = s->input_out = 0;
    s->input_seq = -1;
    s->inputs = 0;
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
    s->open = -1;
    /* The block whole before head says so (one core: the compiler's
     * order is enough). */
    __asm__ volatile("" ::: "memory");
    s->head++;
}

/* The block at head given to channel ch (the one there before, of
 * another channel, goes as it is: its message goes on in a later
 * block).  0, or -1 when the ring has no room for it. */
static int open_for(struct aastream *s, int ch)
{
    struct aastream_channel *c = &s->ch[ch];
    if (s->open == ch)
        return 0;
    if (s->open >= 0)
        publish(s, 0);
    if (s->head - s->tail >= s->blocks)
        return -1;
    s->open = ch;
    s->fill = 0;
    s->flags = (uint16_t)(c->pending | (c->type & 15) << 8);
    /* The key frame's first block: where a decoder may start. */
    if (c->pending & AALINK_KEY)
        s->key = s->head;
    c->pending = 0;
    return 0;
}

void aastream_begin(struct aastream *s, int ch, int type, int key)
{
    struct aastream_channel *c = &s->ch[ch];
    if (c->in_message)
        (void)aastream_end(s, ch);
    c->in_message = 1;
    c->type = (uint8_t)type;
    c->pending = (uint16_t)(AALINK_START | (key ? AALINK_KEY : 0));
    if (type == AALINK_CONFIG) {
        s->config_taking = 1;
        s->config_len = 0;
    }
    if (type == AALINK_PICTURE)
        s->messages++;
    if (!AALINK_IS_SOUND(type))
        s->state |= AALINK_VIDEO;
}

uint32_t aastream_add(struct aastream *s, int ch, const uint8_t *p, uint32_t n)
{
    uint32_t taken = 0;
    if (s->config_taking && s->ch[ch].type == AALINK_CONFIG)
        for (; s->config_len < AALINK_CONFIG_MAX && taken < n; taken++)
            s->config[s->config_len++] = p[taken];
    taken = 0;
    while (taken < n) {
        uint32_t k;
        uint8_t *b;
        if (s->open == ch && s->fill == AALINK_PAYLOAD) {
            /* Full, and more to come: it goes, unless the ring has no
             * room for the next. */
            if (s->head + 1 - s->tail >= s->blocks)
                break;
            publish(s, 0);
        }
        if (open_for(s, ch))
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

uint32_t aastream_end(struct aastream *s, int ch)
{
    struct aastream_channel *c = &s->ch[ch];
    uint32_t seq = s->head;
    if (!c->in_message)
        return seq - 1;
    c->in_message = 0;
    if (c->type == AALINK_CONFIG)
        s->config_taking = 0;
    /* Its last block: the one open for it, or (when another channel's
     * came between, or nothing was added) one of its own with no
     * bytes. */
    if (open_for(s, ch))
        return seq - 1; /* no room even for that: the caller looked first */
    seq = s->head;
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

BRIDGE_RAM int aastream_in_window(uint32_t index, uint32_t *window_index)
{
    if (index < AALINK_CTL_SECTORS || index >= AALINK_INPUT_FIRST)
        return 0;
    *window_index = index - AALINK_CTL_SECTORS;
    return 1;
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
    put32(b + AALINK_CTL_O_INPUTS, s->inputs);
    for (i = 0; i < n; i++)
        b[AALINK_CTL_CONFIG + i] = s->config[i];
    words_of(b, w);
}

/* A read of the input sectors: the event, unless it is the last again
 * (the same code, sequence number and all: a read made again). */
BRIDGE_RAM static void input(struct aastream *s, uint32_t code)
{
    uint32_t n = s->input_in;
    if ((int)code == s->input_seq)
        return;
    s->input_seq = (int)code;
    if (n - s->input_out >= AASTREAM_INPUTS)
        return;
    s->input[n % AASTREAM_INPUTS] = (uint16_t)code;
    __asm__ volatile("" ::: "memory");
    s->input_in = n + 1;
    s->inputs++;
}

int aastream_input_take(struct aastream *s, uint16_t *code)
{
    uint32_t n = s->input_out;
    if (n == s->input_in)
        return 0;
    *code = s->input[n % AASTREAM_INPUTS];
    __asm__ volatile("" ::: "memory");
    s->input_out = n + 1;
    return 1;
}

BRIDGE_RAM void aastream_request(struct aastream *s, uint32_t index)
{
    if (index >= AALINK_INPUT_FIRST && index < AALINK_FILE_SECTORS)
        input(s, index - AALINK_INPUT_FIRST);
    else if (index == AALINK_CTL_KEY)
        s->key_asked++;
}

BRIDGE_RAM void aastream_sector(struct aastream *s, uint32_t index, uint32_t *w)
{
    uint32_t i;
    if (index >= AALINK_INPUT_FIRST && index < AALINK_FILE_SECTORS) {
        state_sector(s, w);
        return;
    }
    if (index < AALINK_CTL_SECTORS) {
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
