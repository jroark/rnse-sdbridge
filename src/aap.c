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
