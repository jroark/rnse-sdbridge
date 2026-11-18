#include "aap.h"

int aap_version_request(uint8_t *out)
{
    static const uint8_t m[] = {
        0x00, AAP_FIRST | AAP_LAST, 0x00, 0x06,    /* channel 0, one frame, 6 bytes */
        0x00, AAP_VERSION_REQUEST, 0x00, 0x01, 0x00, 0x01,
    };
    int i;
    for (i = 0; i < (int)sizeof m; i++)
        out[i] = m[i];
    return (int)sizeof m;
}

void aap_reader_init(struct aap_reader *r)
{
    r->have = 0;
    r->consumed = 0;
}

int aap_feed(struct aap_reader *r, const uint8_t *p, uint32_t n)
{
    uint32_t i;
    if (r->consumed) {
        for (i = r->consumed; i < r->have; i++)
            r->buf[i - r->consumed] = r->buf[i];
        r->have -= r->consumed;
        r->consumed = 0;
    }
    if (n > sizeof r->buf - r->have)
        return -1;
    while (n--)
        r->buf[r->have++] = *p++;
    return 0;
}

int aap_next(struct aap_reader *r, struct aap_frame *f)
{
    uint32_t head, len, i;
    if (r->consumed) {
        for (i = r->consumed; i < r->have; i++)
            r->buf[i - r->consumed] = r->buf[i];
        r->have -= r->consumed;
        r->consumed = 0;
    }
    if (r->have < 4)
        return 0;
    len = (uint32_t)r->buf[2] << 8 | r->buf[3];
    /* A first frame of several carries the message's length as well. */
    head = (r->buf[1] & 3) == AAP_FIRST ? 8 : 4;
    if (r->have < head + len)
        return 0;
    f->channel = r->buf[0];
    f->flags = r->buf[1];
    f->length = (uint16_t)len;
    f->payload = r->buf + head;
    r->consumed = head + len;
    return 1;
}

int aap_version_response(const struct aap_frame *f, uint16_t *major, uint16_t *minor, uint16_t *status)
{
    const uint8_t *p = f->payload;
    if (f->channel != 0 || (f->flags & AAP_ENCRYPTED) || f->length < 8)
        return -1;
    if ((p[0] << 8 | p[1]) != AAP_VERSION_RESPONSE)
        return -1;
    *major = (uint16_t)(p[2] << 8 | p[3]);
    *minor = (uint16_t)(p[4] << 8 | p[5]);
    *status = (uint16_t)(p[6] << 8 | p[7]);
    return 0;
}

int aap_frame(uint8_t *out, uint8_t channel, uint8_t flags, const uint8_t *payload, uint16_t n)
{
    uint16_t i;
    out[0] = channel;
    out[1] = (uint8_t)(flags | AAP_FIRST | AAP_LAST);
    out[2] = (uint8_t)(n >> 8);
    out[3] = (uint8_t)n;
    for (i = 0; i < n; i++)
        out[4 + i] = payload[i];
    return n + 4;
}

void aap_assembler_init(struct aap_assembler *a)
{
    a->len = 0;
    a->open = 0;
}

int aap_assemble(struct aap_assembler *a, uint8_t flags, const uint8_t *p, uint32_t n)
{
    uint32_t i;
    if (flags & AAP_FIRST) {
        a->len = 0;
        a->open = 1;
    } else if (!a->open) {
        return -1;
    }
    if (n > sizeof a->buf - a->len) {
        a->open = 0;
        return -1;
    }
    for (i = 0; i < n; i++)
        a->buf[a->len++] = p[i];
    if (flags & AAP_LAST) {
        a->open = 0;
        return 1;
    }
    return 0;
}

int aap_auth_complete(uint8_t *payload)
{
    /* The id, then the protocol buffer's field 1 (status) = 0. */
    payload[0] = 0x00;
    payload[1] = AAP_AUTH_COMPLETE;
    payload[2] = 0x08;
    payload[3] = 0x00;
    return 4;
}

static int varint(const uint8_t *p, uint32_t n, uint32_t *at, uint64_t *v)
{
    int shift = 0;
    *v = 0;
    while (*at < n && shift < 64) {
        uint8_t b = p[(*at)++];
        *v |= (uint64_t)(b & 0x7f) << shift;
        if (!(b & 0x80))
            return 0;
        shift += 7;
    }
    return -1;
}

int aap_pb_next(const uint8_t *p, uint32_t n, uint32_t *at, struct aap_pb_field *f)
{
    uint64_t key, len;
    if (*at >= n)
        return 0;
    if (varint(p, n, at, &key))
        return -1;
    f->number = (uint32_t)(key >> 3);
    f->type = (uint32_t)(key & 7);
    f->bytes = 0;
    f->len = 0;
    f->value = 0;
    switch (f->type) {
    case 0:
        return varint(p, n, at, &f->value) ? -1 : 1;
    case 1:
        if (n - *at < 8)
            return -1;
        *at += 8;
        return 1;
    case 5:
        if (n - *at < 4)
            return -1;
        *at += 4;
        return 1;
    case 2:
        if (varint(p, n, at, &len) || len > n - *at)
            return -1;
        f->bytes = p + *at;
        f->len = (uint32_t)len;
        *at += (uint32_t)len;
        return 1;
    }
    return -1;
}

void aap_pb_init(struct aap_pb *b, uint8_t *p, uint32_t cap)
{
    b->p = p;
    b->len = 0;
    b->cap = cap;
    b->err = 0;
}

static void put(struct aap_pb *b, uint8_t c)
{
    if (b->len >= b->cap) {
        b->err = 1;
        return;
    }
    b->p[b->len++] = c;
}

static void put_varint(struct aap_pb *b, uint64_t v)
{
    while (v >= 0x80) {
        put(b, (uint8_t)(v | 0x80));
        v >>= 7;
    }
    put(b, (uint8_t)v);
}

void aap_pb_id(struct aap_pb *b, uint16_t id)
{
    put(b, (uint8_t)(id >> 8));
    put(b, (uint8_t)id);
}

void aap_pb_varint(struct aap_pb *b, uint32_t field, uint64_t v)
{
    put_varint(b, (uint64_t)field << 3);
    put_varint(b, v);
}

void aap_pb_bytes(struct aap_pb *b, uint32_t field, const void *p, uint32_t n)
{
    const uint8_t *q = p;
    put_varint(b, (uint64_t)field << 3 | 2);
    put_varint(b, n);
    while (n--)
        put(b, *q++);
}

void aap_pb_string(struct aap_pb *b, uint32_t field, const char *s)
{
    uint32_t n = 0;
    while (s[n])
        n++;
    aap_pb_bytes(b, field, s, n);
}
