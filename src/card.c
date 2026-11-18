#include "card.h"
#include "crc.h"
#include "volume.h"

/* Status bits (SD Physical Layer, "Card Status"). */
#define OUT_OF_RANGE (1u << 31)
#define ADDRESS_ERROR (1u << 30)
#define BLOCK_LEN_ERROR (1u << 29)
#define WP_VIOLATION (1u << 26)
#define COM_CRC_ERROR (1u << 23)
#define ILLEGAL_COMMAND (1u << 22)
#define READY_FOR_DATA (1u << 8)
#define APP_CMD (1u << 5)

#define RCA 0x5d1b

uint8_t card_cid[16], card_csd[16];

/* Bits [hi:lo] of a 128-bit register, the first byte most significant. */
static void field(uint8_t *reg, int hi, int lo, uint32_t v)
{
    int b;
    for (b = lo; b <= hi; b++) {
        uint8_t *p = &reg[15 - b / 8];
        uint8_t m = (uint8_t)(1u << (b % 8));
        if (v >> (b - lo) & 1)
            *p |= m;
        else
            *p &= (uint8_t)~m;
    }
}

static void registers(void)
{
    int i;
    for (i = 0; i < 16; i++)
        card_cid[i] = card_csd[i] = 0;
    /* CID: no manufacturer of note, "RN", "LINKB", revision 1.0, serial
     * 1, made 2026-11. */
    field(card_cid, 127, 120, 0x00);
    field(card_cid, 119, 104, 'R' << 8 | 'N');
    for (i = 0; i < 5; i++)
        field(card_cid, 103 - 8 * i, 96 - 8 * i, (uint32_t)"LINKB"[i]);
    field(card_cid, 63, 56, 0x10);
    field(card_cid, 55, 24, 1);
    field(card_cid, 19, 8, (26u << 4) | 11);
    /* CSD version 1.0 for 64 MiB: (C_SIZE + 1) * 2^(C_SIZE_MULT + 2)
     * blocks of 2^READ_BL_LEN bytes = 256 * 512 * 512. */
    field(card_csd, 127, 126, 0);
    field(card_csd, 119, 112, 0x26);      /* TAAC 1.5 ms */
    field(card_csd, 111, 104, 0);         /* NSAC */
    field(card_csd, 103, 96, 0x32);       /* TRAN_SPEED 25 MHz */
    field(card_csd, 95, 84, 0x5b5);       /* CCC */
    field(card_csd, 83, 80, 9);           /* READ_BL_LEN 512 */
    field(card_csd, 79, 79, 1);           /* READ_BL_PARTIAL */
    field(card_csd, 73, 62, 255);         /* C_SIZE */
    field(card_csd, 61, 59, 7);           /* VDD_R_CURR_MIN */
    field(card_csd, 58, 56, 6);           /* VDD_R_CURR_MAX */
    field(card_csd, 55, 53, 7);
    field(card_csd, 52, 50, 6);
    field(card_csd, 49, 47, 7);           /* C_SIZE_MULT */
    field(card_csd, 46, 46, 1);           /* ERASE_BLK_EN */
    field(card_csd, 45, 39, 0x7f);        /* SECTOR_SIZE */
    field(card_csd, 28, 26, 2);           /* R2W_FACTOR */
    field(card_csd, 25, 22, 9);           /* WRITE_BL_LEN */
    field(card_csd, 12, 12, 1);           /* TMP_WRITE_PROTECT */
    card_cid[15] = (uint8_t)(crc7(card_cid, 15) << 1 | 1);
    card_csd[15] = (uint8_t)(crc7(card_csd, 15) << 1 | 1);
}

void card_reset(struct card *c)
{
    static int done;
    if (!done) {
        crc_init();
        registers();
        done = 1;
    }
    c->state = IDLE;
    c->app = 0;
    c->polls = 0;
    c->rca = 0;
    c->errors = 0;
}

void card_frame(uint32_t w0, uint32_t w1, uint8_t *cmd)
{
    cmd[0] = (uint8_t)(w0 >> 25);
    cmd[1] = (uint8_t)(w0 >> 17);
    cmd[2] = (uint8_t)(w0 >> 9);
    cmd[3] = (uint8_t)(w0 >> 1);
    cmd[4] = (uint8_t)((w0 & 1) << 7 | w1 >> 25);
    cmd[5] = (uint8_t)(w1 >> 17);
}

/* Bytes into the response words. */
static void pack(struct reply *r, const uint8_t *b, int n)
{
    int i;
    r->words[0] = (uint32_t)(8 * n - 1);
    for (i = 1; i < 6; i++)
        r->words[i] = 0;
    for (i = 0; i < n; i++)
        r->words[1 + i / 4] |= (uint32_t)b[i] << (24 - 8 * (i % 4));
    r->nwords = 1 + (n + 3) / 4;
}

static void short_response(struct reply *r, uint8_t index, uint32_t v, int crc)
{
    uint8_t b[6];
    b[0] = index & 0x3f;
    b[1] = (uint8_t)(v >> 24);
    b[2] = (uint8_t)(v >> 16);
    b[3] = (uint8_t)(v >> 8);
    b[4] = (uint8_t)v;
    b[5] = crc ? (uint8_t)(crc7(b, 5) << 1 | 1) : 0xff;
    pack(r, b, 6);
}

static void long_response(struct reply *r, const uint8_t *reg)
{
    uint8_t b[17];
    int i;
    b[0] = 0x3f;
    for (i = 0; i < 16; i++)
        b[1 + i] = reg[i];
    pack(r, b, 17);
}

/* R1: the errors so far (cleared by being said) and the state the
 * command found. */
static void r1(struct card *c, struct reply *r, uint8_t index, int state, uint32_t more)
{
    uint32_t s = c->errors | more | (uint32_t)state << 9 | (state == TRAN ? READY_FOR_DATA : 0) | (c->app ? APP_CMD : 0);
    c->errors = 0;
    short_response(r, index, s, 1);
}

static int addressed(const struct card *c, uint32_t arg)
{
    return (arg >> 16) == c->rca;
}

void card_command(struct card *c, const uint8_t *cmd, struct reply *r)
{
    uint8_t index = cmd[0] & 0x3f;
    uint32_t arg = (uint32_t)cmd[1] << 24 | (uint32_t)cmd[2] << 16 | (uint32_t)cmd[3] << 8 | cmd[4];
    int app = c->app, state = c->state;
    r->nwords = 0;
    r->read = 0;
    r->index = index;
    /* Not a command (a response on the line, or noise), or damaged:
     * no answer, and a damaged one is said with the next. */
    if ((cmd[0] & 0xc0) != 0x40 || !(cmd[5] & 1))
        return;
    if (crc7(cmd, 5) != cmd[5] >> 1) {
        c->bad_crc++;
        c->errors |= COM_CRC_ERROR;
        return;
    }
    c->commands++;
    c->app = 0;
    if (app) {
        switch (index) {
        case 41: /* SD_SEND_OP_COND */
            if (state != IDLE && state != READY)
                break;
            {
                uint32_t ocr = 0x00ff8000;
                /* An inquiry (no voltages) does not start the card. */
                if (arg & 0x00ff8000) {
                    if (++c->polls >= 2) {
                        ocr |= 0x80000000;
                        c->state = READY;
                    }
                }
                short_response(r, 0x3f, ocr, 0);
            }
            return;
        case 6: /* SET_BUS_WIDTH: one line only, and nobody asks for four */
            if (state == TRAN && (arg & 3) == 0) {
                r1(c, r, index, state, APP_CMD);
                return;
            }
            break;
        case 42: /* SET_CLR_CARD_DETECT */
            if (state == TRAN) {
                r1(c, r, index, state, APP_CMD);
                return;
            }
            break;
        default:
            /* Others as the same command without CMD55, as cards do. */
            goto plain;
        }
        c->errors |= ILLEGAL_COMMAND;
        c->illegal++;
        return;
    }
plain:
    switch (index) {
    case 0:
        card_reset(c);
        return;
    case 8: /* SEND_IF_COND: 2.7-3.6 V, the check pattern back */
        if (state == IDLE && ((arg >> 8) & 0xf) == 1) {
            short_response(r, 8, arg & 0xfff, 1);
            return;
        }
        break;
    case 55:
        if (state == IDLE || state == READY || state == IDENT || addressed(c, arg) || state == STBY || state == TRAN) {
            c->app = 1;
            r1(c, r, index, state, 0);
            return;
        }
        break;
    case 2:
        if (state == READY) {
            c->state = IDENT;
            long_response(r, card_cid);
            return;
        }
        break;
    case 3:
        if (state == IDENT || state == STBY) {
            uint32_t s = c->errors;
            c->rca = RCA;
            c->state = STBY;
            c->errors = 0;
            short_response(r, 3, (uint32_t)RCA << 16 | (s >> 8 & 0xc000) | (s >> 6 & 0x2000) | (uint32_t)state << 9 | (s & 0x1ff & ~READY_FOR_DATA), 1);
            return;
        }
        break;
    case 9:
    case 10:
        if (!addressed(c, arg) && c->rca)
            return; /* for another card: nothing to say, nothing wrong */
        if (state == STBY && addressed(c, arg)) {
            long_response(r, index == 9 ? card_csd : card_cid);
            return;
        }
        break;
    case 7:
        if (addressed(c, arg) && c->rca) {
            if (state == STBY)
                c->state = TRAN;
            r1(c, r, index, state, 0);
        } else if (state == TRAN || state == DATA) {
            /* Another card selected: this one goes to stand-by, saying
             * nothing. */
            c->state = STBY;
        }
        return;
    case 13:
        if (!addressed(c, arg) && c->rca)
            return;
        if (addressed(c, arg) && state >= STBY) {
            r1(c, r, index, state, 0);
            return;
        }
        break;
    case 16:
        if (state == TRAN) {
            r1(c, r, index, state, arg == 512 ? 0 : BLOCK_LEN_ERROR);
            return;
        }
        break;
    case 17:
    case 18:
        if (state == TRAN) {
            uint32_t more = 0;
            if (arg & 511)
                more = ADDRESS_ERROR;
            else if (arg / 512 >= VOLUME_SECTORS)
                more = OUT_OF_RANGE;
            r1(c, r, index, state, more);
            if (!more) {
                r->read = index == 17 ? 1 : 2;
                r->sector = arg / 512;
                if (index == 18)
                    c->state = DATA;
            }
            return;
        }
        break;
    case 12:
        if (state == DATA || state == TRAN) {
            r1(c, r, index, state, 0);
            c->state = TRAN;
            r->read = -1;
            return;
        }
        break;
    case 24:
    case 25:
    case 27:
    case 28:
    case 29:
    case 32:
    case 33:
    case 38:
        /* Writing, protecting, erasing: refused, the card stays where
         * it is. */
        if (state == TRAN) {
            r1(c, r, index, state, WP_VIOLATION);
            return;
        }
        break;
    }
    c->errors |= ILLEGAL_COMMAND;
    c->illegal++;
}

void card_block(const uint32_t *sector, uint32_t *out)
{
    int i;
    out[0] = 4096 + 16 - 1;
    for (i = 0; i < 128; i++)
        out[1 + i] = sector[i];
    out[129] = (uint32_t)crc16_words(sector, 128) << 16;
}
