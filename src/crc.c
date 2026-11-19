#include "crc.h"

static uint8_t t7[256];
static uint16_t t16[256];

void crc_init(void)
{
    int i, b;
    for (i = 0; i < 256; i++) {
        /* CRC7: x^7 + x^3 + 1, a byte at a time, the CRC kept in the
         * top seven bits of a byte. */
        uint8_t c = (uint8_t)i;
        uint16_t d = (uint16_t)(i << 8);
        for (b = 0; b < 8; b++) {
            c = c & 0x80 ? (uint8_t)((c << 1) ^ 0x12) : (uint8_t)(c << 1);
            d = d & 0x8000 ? (uint16_t)((d << 1) ^ 0x1021) : (uint16_t)(d << 1);
        }
        t7[i] = c;
        t16[i] = d;
    }
}

BRIDGE_RAM uint8_t crc7(const uint8_t *p, int n)
{
    uint8_t c = 0;
    while (n--)
        c = t7[c ^ *p++];
    return c >> 1;
}

BRIDGE_RAM uint16_t crc16_words(const uint32_t *w, int n)
{
    uint16_t c = 0;
    while (n--) {
        uint32_t v = *w++;
        c = (uint16_t)((c << 8) ^ t16[(c >> 8) ^ (v >> 24)]);
        c = (uint16_t)((c << 8) ^ t16[(c >> 8) ^ ((v >> 16) & 0xff)]);
        c = (uint16_t)((c << 8) ^ t16[(c >> 8) ^ ((v >> 8) & 0xff)]);
        c = (uint16_t)((c << 8) ^ t16[(c >> 8) ^ (v & 0xff)]);
    }
    return c;
}
