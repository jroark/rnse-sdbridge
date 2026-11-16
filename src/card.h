/* An SD memory card's side of the command line, as far as a host that
 * reads needs it: the start-up (CMD0, 8, 55 + ACMD41, 2, 3, 9, 10, 7),
 * CMD13, 16, 17, 18, 12, and the writes refused (the card says it is
 * write-protected).  Standard capacity (byte addresses), one data line.
 *
 * Plain C with no SDK: the RP2040 calls card_command from its command
 * loop, sdbridge/tests/host.c from a test. */
#ifndef CARD_H
#define CARD_H
#include <stdint.h>

enum { IDLE, READY, IDENT, STBY, TRAN, DATA };

struct card {
    int state;
    int app;                /* the last command was CMD55 */
    int polls;              /* ACMD41s seen: ready from the second */
    uint16_t rca;
    uint32_t errors;        /* status bits for the next response */
    uint32_t commands, bad_crc, illegal;
};

/* What to do about one command. */
struct reply {
    /* The response for the command state machine: words[0] is its
     * length in bits less one (47 or 135), the rest its bits, most
     * significant first; nwords words in all.  0 = no response. */
    uint32_t words[6];
    int nwords;
    /* 1 a block, 2 blocks until stopped, from `sector`; -1 stop. */
    int read;
    uint32_t sector;
    uint8_t index;
};

void card_reset(struct card *c);
/* The command's six bytes, its CRC and end bit included. */
void card_command(struct card *c, const uint8_t *cmd, struct reply *r);
/* The six bytes out of the two words the command state machine pushes:
 * bits 46-15 of the frame, then bits 14-0 at the top of the second. */
void card_frame(uint32_t w0, uint32_t w1, uint8_t *cmd);
/* A sector (volume_sector's words) as the data state machine takes it:
 * the number of bits less one (4111), the 128 words, the CRC16 at the
 * top of a last word.  130 words. */
void card_block(const uint32_t *sector, uint32_t *out);
#define CARD_BLOCK_WORDS 130

/* The registers, 16 bytes each with their CRC7 and end bit. */
extern uint8_t card_cid[16], card_csd[16];

#endif
