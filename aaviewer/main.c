/* Android Auto's picture on the 192: the phone's H.264, from the SD
 * bridge in a card slot (sdbridge/, SDLINK.UF2), decoded and shown.
 *
 * Started by sdk/examples/aaview (AAVIEW.BIN in sdboot's menu, this as
 * AAVIEWER.IMG beside it), which loads it to 0x8c010080 (its file
 * `place`): the decoder makes it bigger than the loader's place.
 *
 * The bridge's card holds AALINK.BIN, a window onto the phone's video
 * as a stream of 512-byte blocks (aalink.h has the format).  The program
 * finds the slot with that file (through the installed software's card
 * program if it is there, else through its own), takes that slot's card
 * processor over with sdk/examples/linkbench's program (its own card
 * commands: CMD18 in runs, the words to the main processor in bursts,
 * as linkbench measured 2 MB/s clean on a unit), and reads the blocks
 * in order: a run of up to RUN blocks as far as the bridge has them
 * (each block says how far that is), the bytes of each message
 * gathered, each picture decoded whole (third_party/h264bsd, as
 * examples/h264bench) and shown at half size, 400x240 of which the
 * screen has 235 lines, in 8 bits a pixel through a palette of 3-3-2
 * bits.
 *
 * Reading a block tells the bridge that what came before it has been
 * taken; the bridge then lets the phone send its next picture.  So the
 * phone goes at the pace this decodes, and nothing is dropped.  Once a
 * message is read whole the next read starts at once, a single block,
 * so that the bridge has its room back while this decodes.
 *
 * A block that is wrong (its sum, its number, or the card processor
 * saying it did not come) is read again.  Started while the video is
 * under way, the program asks the bridge for a key frame (its second
 * control sector) when the last one is gone, and skips pictures until
 * one comes.
 *
 * The console (the debug port) has a line every two seconds: pictures,
 * frames a second, the link's KB/s, milliseconds decoding, showing and
 * reading per picture, blocks read again.  Built with -DAA_CRC it has
 * the CRC-32 of the frames so far after each (tests/aaviewer.rs).
 *
 * The keys are answered all the while: the display key and CD EJECT
 * work, and RETURN held for two seconds goes back to the SD loader (the
 * bridge's card processor back into reset, the caches as they were).
 *
 * The 192 only. */
#include "rnse.h"
#include "h264bsd_decoder.h"
#include "aalink.h"
#include "../aaview/handoff.h"
#include "../linkbench/program.h"

#define STATUS 0xb2400001 /* bits 2, 0: nothing to read; 3, 1: cannot take a word */
#define LATCH 0xb6000004  /* bits 2, 3: the processors' reset lines, low = reset */
static const u32 ports[2] = { 0xb5000002, 0xb500004a };

#define CCR 0xff00001c
#define MMUCR 0xff000010
#define VRAM 0xb8000000
#define SURFACE 0x00110000
#define STRIDE 448

/* The decoder's memory, through P0, above sdboot's session and below
 * aaview (in the loader's place, 0x8df00000). */
#define HEAP 0x0c800000
#define HEAP_END 0x0dcf0000

#define RUN 32                       /* blocks a read at most */
#define MSG_ROOM (1u << 20)          /* the messages read and not yet decoded */
#define DONE_MAX 64

#ifndef BURST_PACE
#define BURST_PACE 20                /* as linkbench: see block_in there */
#endif

/* The card interface's clock: the processor's / 4, as linkbench found
 * clean in bursts (CMD18 /4 BURST, 2 MB/s on a unit). */
#define CTL(i) ((u16)((i) << 10 | 2))
#define CTL_SLOW CTL(14)
#define CTL_FAST CTL(3)

#define BLACK 0x00
#define WHITE 0xff

static int slot = -1;
static u32 port;
static u8 none_bit, full_bit;
static int dead, high_capacity;
static u16 pins_data, pins_dir;
static int pins_known;
static u32 found_ccr;
static u32 link_first;               /* AALINK.BIN's first sector */
static u32 chain[9000];

/* ---- the screen before the picture --------------------------------- */

static int row, showing;

static char *put_dec(char *p, u32 v)
{
    char t[10];
    int n = 0;
    do {
        t[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n)
        *p++ = t[--n];
    return p;
}

static char *put_str(char *p, const char *s)
{
    while (*s)
        *p++ = *s++;
    return p;
}

static void line(const char *text)
{
    int y = 2 + 10 * row;
    console_puts(text);
    console_puts("\n");
    if (showing)
        return;
    screen_fill(0, y - 1, screen_width, 10, BLACK);
    screen_text(2, y, text, WHITE, 1);
    if (++row >= 23)
        row = 0;
}

static void pause(u32 ms)
{
    u32 t = millis();
    while (millis() - t < ms) {
        panel_service();
        cpu_idle();
    }
}

/* ---- colour --------------------------------------------------------- */

static u8 clamp_table[1024];
#define CLAMP(v) clamp_table[(v) + 384]
/* Colour by 5 bits of luma and 4 each of the two chroma samples, one
 * look-up a pixel (as examples/h264bench). */
static u8 colour[32 * 16 * 16];
#define PIXEL(y, cb, cr) colour[((y) >> 3) << 8 | ((cb) >> 4) << 4 | (cr) >> 4]

static void tables(void)
{
    u32 i;
    s32 k;
    for (k = -384; k < 640; k++)
        clamp_table[k + 384] = (u8)(k < 0 ? 0 : k > 255 ? 255 : k);
    /* ITU-R BT.601, studio range, in 1/256, for the middle of each step. */
    for (i = 0; i < sizeof colour; i++) {
        s32 y = (s32)(i >> 8) * 8 + 4, cb = (s32)((i >> 4) & 15) * 16 + 8, cr = (s32)(i & 15) * 16 + 8;
        s32 l = (y - 16) * 298 >> 8;
        u32 r = CLAMP(l + ((409 * (cr - 128)) >> 8));
        u32 g = CLAMP(l - ((208 * (cr - 128)) >> 8) - ((100 * (cb - 128)) >> 8));
        u32 b = CLAMP(l + ((516 * (cb - 128)) >> 8));
        colour[i] = (u8)((r & 0xe0) | (g & 0xe0) >> 3 | b >> 6);
    }
}

static void palette(void)
{
    u32 i;
    for (i = 0; i < 256; i++)
        screen_palette((int)i, (u8)((i >> 5) * 255 / 7), (u8)(((i >> 2) & 7) * 255 / 7), (u8)((i & 3) * 255 / 3));
}

/* Every other pixel of every other line, as examples/h264bench shows. */
static void show(const u8 *pic, u32 w, u32 h)
{
    const u8 *cb = pic + w * h, *cr = cb + w * h / 4;
    u32 x, y, lines = (u32)screen_height < h / 2 ? (u32)screen_height : h / 2, cols = w / 2 > 400 ? 400 : w / 2;
    for (y = 0; y < lines; y++) {
        const u8 *py = pic + 2 * y * w, *pb = cb + y * (w / 2), *pr = cr + y * (w / 2);
        volatile u32 *out = (volatile u32 *)(VRAM + SURFACE + y * STRIDE);
        for (x = 0; x < cols; x += 4) {
            *out++ = (u32)PIXEL(py[0], pb[0], pr[0]) | (u32)PIXEL(py[2], pb[1], pr[1]) << 8 |
                     (u32)PIXEL(py[4], pb[2], pr[2]) << 16 | (u32)PIXEL(py[6], pb[3], pr[3]) << 24;
            py += 8;
            pb += 4;
            pr += 4;
        }
    }
}

#ifdef AA_CRC
static u32 crc_table[256];

static void crc_init(void)
{
    u32 i, j, c;
    for (i = 0; i < 256; i++) {
        for (c = i, j = 0; j < 8; j++)
            c = c & 1 ? 0xedb88320 ^ (c >> 1) : c >> 1;
        crc_table[i] = c;
    }
}

static u32 crc32(u32 crc, const u8 *p, u32 n)
{
    crc = ~crc;
    while (n--)
        crc = crc_table[(crc ^ *p++) & 0xff] ^ (crc >> 8);
    return ~crc;
}
#endif

/* ---- the card processor (sdk/examples/linkbench's program) --------- */

static int put(u16 w)
{
    if (REG8(STATUS) & full_bit) {
        u32 t = millis();
        while (REG8(STATUS) & full_bit)
            if (millis() - t > 500) {
                dead = 1;
                return -1;
            }
    }
    REG16(port) = w;
    return 0;
}

static int get(u16 *w, u32 ms)
{
    if (REG8(STATUS) & none_bit) {
        u32 t = millis();
        while (REG8(STATUS) & none_bit)
            if (millis() - t > ms) {
                dead = 1;
                return -1;
            }
    }
    *w = REG16(port);
    return 0;
}

static int done_word(void)
{
    u16 w;
    if (get(&w, 1000))
        return -1;
    if (w != 0xf000) {
        dead = 1;
        return -1;
    }
    return 0;
}

static void dsp_reset(void)
{
    if (slot >= 0)
        REG32(LATCH) &= ~(4u << slot);
}

/* The program in, as linkbench loads it: the boot ROM's download, then
 * its hello and a ping. */
static int dsp_load(void)
{
    u32 i, count = sizeof program / sizeof program[0];
    u16 w;
    dead = 0;
    REG32(LATCH) &= ~(4u << slot);
    delay_ms(20);
    REG32(LATCH) |= 4u << slot;
    delay_ms(20);
    if (put(1) || put((u16)count) || put(0x0c00))
        return -1;
    for (i = 0; i < count; i++)
        if (put((u16)program[i]) || put((u16)(program[i] >> 16)))
            return -2;
    if (get(&w, 500) || w != 0x1b01)
        return -3;
    if (put(1) || get(&w, 100) || w != 0xa5a5)
        return -4;
    return 0;
}

static int wrreg(u16 a, u16 v)
{
    return put(2) || put(a) || put(v) || done_word() ? -1 : 0;
}

static int setctl(u16 v)
{
    return put(3) || put(v) || done_word() ? -1 : 0;
}

/* A card command: the header (SDCTL after the response, 0x80 when none
 * came), or -1; the response's words in r (3 for a short one, 8 for a
 * long one: the response register goes round, linkbench's card_cmd). */
static int card_cmd(u16 reg, u32 arg, u16 *r)
{
    u16 h;
    int i, n = reg & 0x40 ? 8 : 3;
    if (put(5) || put(reg) || put((u16)(arg >> 16)) || put((u16)arg) || get(&h, 1000))
        return -1;
    for (i = 0; i < 8; i++)
        r[i] = 0;
    for (i = 0; i < n; i++)
        if (get(&r[i], 100))
            return -1;
    return h;
}

static int responded(int h)
{
    return h >= 0 && !(h & 0x80);
}

static u32 r32(const u16 *r)
{
    return (u32)(r[0] & 0xff) << 24 | (u32)r[1] << 8 | r[2] >> 8;
}

static u32 card_pace = 20;
static u16 start_ctl;

/* A command of the start, after the interface's setting written again
 * (which puts the response register's read position back), as
 * linkbench's start_cmd. */
static int start_cmd(u16 reg, u32 arg, u16 *r)
{
    if (setctl(start_ctl))
        return -1;
    return card_cmd(reg, arg, r);
}

/* The card's start, as linkbench's card_start: 0, or the step that
 * failed. */
static int card_start(void)
{
    u16 r[8];
    u32 hcs, t, ocr, rca;
    int h;
    start_ctl = CTL_SLOW;
    if (setctl(CTL_SLOW))
        return 1;
    pause(card_pace);
    if (card_cmd(0x4800, 0, r) < 0)                          /* CMD0 */
        return 2;
    pause(card_pace);
    h = start_cmd(0x4c08, 0x1aa, r);                          /* CMD8 */
    if (h < 0)
        return 3;
    hcs = responded(h) && (r32(r) & 0xfff) == 0x1aa ? 0x40000000 : 0;
    t = millis();
    for (;;) {
        pause(card_pace);
        if (!responded(start_cmd(0x4c37, 0, r)))              /* CMD55 */
            return 4;
        if (!responded(start_cmd(0x4c29, hcs | 0x00ff0000, r))) /* ACMD41 */
            return 5;
        ocr = r32(r);
        if (ocr & 0x80000000)
            break;
        if (millis() - t > 6000)
            return 6;
    }
    high_capacity = (ocr & 0x40000000) != 0;
    pause(card_pace);
    if (!responded(start_cmd(0x4c42, 0, r)))                  /* CMD2 */
        return 7;
    if (!responded(start_cmd(0x4c03, 0, r)))                  /* CMD3 */
        return 8;
    rca = (u32)(r[0] & 0xff) << 8 | r[1] >> 8;
    (void)start_cmd(0x4c49, rca << 16, r);                    /* CMD9: not needed */
    start_ctl = CTL_FAST;
    if (setctl(CTL_FAST))
        return 10;
    if (!responded(start_cmd(0x4c07, rca << 16, r)))          /* CMD7 */
        return 11;
    if (!responded(start_cmd(0x4c10, 0x200, r)))              /* CMD16 */
        return 12;
    return 0;
}

/* The program loaded, the pins as the installed program had them, the
 * card started; once more at the stock driver's pace if that failed. */
static int take_over(void)
{
    int r, k;
    port = ports[slot];
    none_bit = slot ? 1 : 4;
    full_bit = slot ? 2 : 8;
    for (k = 0; k < 2; k++) {
        card_pace = k ? 150 : 20;
        r = dsp_load();
        if (r)
            r = 100 - r;
        else if (wrreg(0x0f70, 0) || wrreg(0x0f72, 0) || wrreg(0x0f73, 0))
            r = 111;
        else if (pins_known && (wrreg(0x3804, (u16)((pins_data | 4) & 0xff)) || wrreg(0x3805, (u16)(0x3000 | (pins_dir & 0xff)))))
            r = 110;
        else
            r = card_start();
        if (!r)
            return 0;
        dsp_reset();
        pause(200);
    }
    return r;
}

static u32 address(u32 sector)
{
    return high_capacity ? sector : sector << 9;
}

/* One block: its header, its 256 words into buf; the words after the
 * first in a burst, not looked at (linkbench's block_in). */
static int block_in(u16 *buf)
{
    volatile u16 *p = (volatile u16 *)port;
    u16 h;
    int i, d;
    if (get(&h, 500) || get(&buf[0], 100))
        return -1;
    for (i = 1; i < 256; i++) {
        for (d = 0; d < BURST_PACE; d++)
            __asm__ volatile("nop");
        buf[i] = *p;
    }
    return h;
}

static u32 late_blocks, missing_blocks, stray_status;

/* n sectors from `sector` by CMD18 into to: 0, or -1 when the processor
 * did not answer as it should.  Blocks that did not come have their
 * header's 0x80 (counted; they read as whatever was in the buffer). */
static u8 block_ok[RUN];

static int read_run(u32 sector, u32 n, u8 *to)
{
    u32 a = address(sector), i;
    u16 h, late;
    int b;
    if (put(7) || put((u16)(a >> 16)) || put((u16)a) || put((u16)n))
        return -1;
    for (i = 0; i < n; i++) {
        b = block_in((u16 *)(to + 512 * i));
        if (b < 0)
            return -1;
        block_ok[i] = !(b & 0x80);
        if (b & 0x80)
            missing_blocks++;
    }
    if (get(&h, 1000) || get(&late, 100))
        return -1;
    stray_status |= h;
    late_blocks += late;
    return 0;
}

/* One sector by CMD17, for the FAT code and the control sectors. */
static int read_one(u32 sector, void *buf)
{
    static u16 bounce[256];
    u32 a = address(sector), step = high_capacity ? 1 : 512;
    int h;
    if (dead)
        return -1;
    if (put(6) || put((u16)(a >> 16)) || put((u16)a) || put((u16)(step >> 16)) || put((u16)step) || put(1))
        return -1;
    h = block_in(bounce);
    if (h < 0 || done_word() || (h & 0x80))
        return -1;
    memcpy(buf, bounce, 512);
    return 0;
}

/* ---- finding the bridge -------------------------------------------- */

static u32 be32(const u8 *p)
{
    return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3];
}

static u32 be16(const u8 *p)
{
    return (u32)p[0] << 8 | p[1];
}

/* AALINK.BIN on the card that sd_read reads now: its first sector, when
 * it is there and in one run. */
static int find_link(void)
{
    struct sd_file f;
    u32 i;
    if (sd_open(AALINK_NAME, &f, chain, sizeof chain / sizeof chain[0]) || f.size < AALINK_FILE_SECTORS * 512u)
        return 0;
    for (i = 1; i < f.clusters; i++)
        if (chain[i] != chain[i - 1] + 1)
            return 0;
    link_first = f.data_start + (chain[0] - 2) * f.per_cluster;
    return 1;
}

static void leave(void)
{
    sd_reader = 0;
    dsp_reset();
    cache_setup(found_ccr);
}

/* What a slot said, for the screen: SLOT n: then the words. */
static void slot_says(int s, const char *what, int r)
{
    char text[64], *q = put_str(text, "SLOT ");
    q = put_dec(q, (u32)s + 1);
    q = put_str(q, ": ");
    q = put_str(q, what);
    if (r) {
        *q++ = ' ';
        if (r < 0) {
            *q++ = '-';
            r = -r;
        }
        q = put_dec(q, (u32)r);
    }
    *q = 0;
    line(text);
}

/* The slot with the bridge, and its card processor ours: 0, or -1. */
static int open_bridge(void)
{
    int s, r, order[2] = { 1, 0 };
    for (int k = 0; k < 2; k++) {
        s = order[k];
        if (!sd_present(s))
            continue;
        slot = s;
        pins_known = 0;
        sd_reader = 0;
        /* Through the installed software's program, if it is there. */
        if (sd_attach(s) == 0 || sd_init(s) == 0) {
            if (!find_link()) {
                slot_says(s, "NO AALINK.BIN", 0);
                continue;
            }
            pins_known = sd_peek(0, 0x3804, &pins_data) == 0 && sd_peek(0, 0x3805, &pins_dir) == 0;
            r = take_over();
            if (r) {
                slot_says(s, "THE BRIDGE'S CARD DID NOT START, STEP", r);
                dsp_reset();
                continue;
            }
            sd_reader = read_one;
            return 0;
        }
        /* Else through this program's. */
        r = take_over();
        if (r) {
            slot_says(s, "THE CARD DID NOT START, STEP", r);
            dsp_reset();
            continue;
        }
        sd_reader = read_one;
        if (find_link())
            return 0;
        slot_says(s, "NO AALINK.BIN", 0);
        sd_reader = 0;
        dsp_reset();
    }
    line("NO BRIDGE (AALINK.BIN) IN EITHER SLOT");
    slot = -1;
    return -1;
}

/* ---- the stream ------------------------------------------------------ */

static u8 run_buf[RUN * 512] __attribute__((aligned(32)));
static u8 messages[MSG_ROOM] __attribute__((aligned(32)));
static u32 msg_used;                 /* bytes of messages[] in use */
static u32 msg_start;                /* where the message being read began */
static int in_message, skipping, waiting_key, msg_type;
static struct {
    u32 at, len;
    int type;
} done[DONE_MAX];
static int ndone;

static u32 next_seq, head_hint;

static u32 window_sector(u32 seq)
{
    return link_first + AALINK_CTL_SECTORS + seq % AALINK_WINDOW;
}

/* Counts for the console. */
static u32 pictures, bytes_in, blocks_in, again, decode_us, show_us, read_us, waits, skipped, frames;
static u32 last_report;

/* A block of a run: 1 taken, 0 not yet there, -1 wrong. */
static int take(const u8 *b)
{
    u32 flags, used, sum = 0, i, seq = be32(b + 4), head = be32(b + 8);
    if (be32(b) != AALINK_BLOCK_MAGIC || seq != next_seq)
        return -1;
    if ((s32)(head - head_hint) > 0)
        head_hint = head;
    flags = be16(b + 12);
    if (!(flags & AALINK_READY))
        return 0;
    used = be16(b + 14);
    for (i = 0; i < AALINK_PAYLOAD; i += 4)
        sum += be32(b + AALINK_HEADER + i);
    if (used > AALINK_PAYLOAD || sum != be32(b + 16))
        return -1;
    if (flags & AALINK_START) {
        /* A message cut short (should not happen): what came is dropped. */
        if (in_message)
            msg_used = msg_start;
        in_message = 1;
        msg_type = (int)AALINK_TYPE(flags);
        msg_start = msg_used;
        /* Waiting for a key frame: a picture before it would not decode. */
        skipping = waiting_key && msg_type == AALINK_PICTURE && !(flags & AALINK_KEY);
        if (msg_type == AALINK_PICTURE && (flags & AALINK_KEY))
            waiting_key = 0;
        if (skipping)
            skipped++;
    }
    if (in_message && !skipping) {
        if (msg_used + used > MSG_ROOM) {
            /* No room (a message of more than a megabyte): dropped. */
            skipping = 1;
            msg_used = msg_start;
        } else {
            memcpy(messages + msg_used, b + AALINK_HEADER, used);
            msg_used += used;
        }
    }
    if ((flags & AALINK_END) && in_message) {
        in_message = 0;
        if (!skipping && ndone < DONE_MAX) {
            done[ndone].at = msg_start;
            done[ndone].len = msg_used - msg_start;
            done[ndone].type = msg_type;
            ndone++;
        } else {
            msg_used = msg_start;
        }
        skipping = 0;
    }
    next_seq++;
    blocks_in++;
    bytes_in += used;
    return 1;
}

/* Reads from next_seq as far as the bridge has blocks (one when it seems
 * to have none): the blocks taken. */
static int read_blocks(u32 want)
{
    u32 n = want, left = AALINK_WINDOW - next_seq % AALINK_WINDOW, i, t = micros();
    int took = 0, r;
    if (n > RUN)
        n = RUN;
    if (n > left)
        n = left;
    if (n == 0)
        n = 1;
    if (read_run(window_sector(next_seq), n, run_buf))
        return -1;
    for (i = 0; i < n; i++) {
        if (!block_ok[i]) {
            again++;
            break;
        }
        r = take(run_buf + 512 * i);
        if (r < 0)
            again++;
        if (r <= 0)
            break;
        took++;
    }
    read_us += micros() - t;
    return took;
}

/* ---- decoding ------------------------------------------------------ */

static storage_t *dec;
static u32 width, height;
#ifdef AA_CRC
static u32 crc;
#endif

static void decode(u8 *p, u32 len)
{
    u32 t0 = micros();
    while (len > 0) {
        u32 r, read, id, idr, errs;
        u8 *pic;
        r = h264bsdDecode(dec, p, len, 0, &read);
        panel_service();
        /* (After the parameter sets the decoder says so and takes
         * nothing: the same bytes again then.) */
        if (read > len || (read == 0 && r != H264BSD_HDRS_RDY))
            break;
        p += read;
        len -= read;
        if (r == H264BSD_MEMALLOC_ERROR) {
            line("OUT OF MEMORY FOR THE DECODER");
            break;
        }
        if (r != H264BSD_PIC_RDY)
            continue;
        width = h264bsdPicWidth(dec) * 16;
        height = h264bsdPicHeight(dec) * 16;
        while ((pic = h264bsdNextOutputPicture(dec, &id, &idr, &errs)) != 0) {
            u32 t1 = micros();
            decode_us += t1 - t0;
#ifdef AA_CRC
            crc = crc32(crc, pic, width * height * 3 / 2);
            console_puts("aaviewer: frame ");
            console_dec(frames);
            console_puts(" crc ");
            console_hex(crc);
            console_puts("\n");
#endif
            if (!showing) {
                showing = 1;
                palette();
            }
            show(pic, width, height);
            frames++;
            t0 = micros();
            show_us += t0 - t1;
        }
    }
    decode_us += micros() - t0;
}

static void decode_done(void)
{
    int i;
    for (i = 0; i < ndone; i++) {
        if (done[i].type == AALINK_PICTURE)
            pictures++;
        decode(messages + done[i].at, done[i].len);
    }
    ndone = 0;
    /* The message being read, if any, to the front. */
    if (in_message && !skipping && msg_start > 0) {
        /* (Forward: the copy is below what it copies.) */
        u32 i, n = msg_used - msg_start;
        for (i = 0; i < n; i++)
            messages[i] = messages[msg_start + i];
        msg_used = n;
        msg_start = 0;
    } else if (!in_message || skipping) {
        msg_used = msg_start = 0;
    }
}

static void report(void)
{
    char text[120], *q = text;
    u32 now = millis(), dt = now - last_report;
    static u32 last_frames, last_bytes;
    if (dt < 2000)
        return;
    q = put_str(q, "aaviewer: ");
    q = put_dec(q, frames);
    q = put_str(q, " frames, ");
    q = put_dec(q, (frames - last_frames) * 10000 / dt);
    q = put_str(q, " fps x10, ");
    q = put_dec(q, (bytes_in - last_bytes) / dt);
    q = put_str(q, " KB/s; per frame ms: decode ");
    q = put_dec(q, frames ? decode_us / 1000 / frames : 0);
    q = put_str(q, " show ");
    q = put_dec(q, frames ? show_us / 1000 / frames : 0);
    q = put_str(q, " read ");
    q = put_dec(q, frames ? read_us / 1000 / frames : 0);
    q = put_str(q, "; again ");
    q = put_dec(q, again);
    q = put_str(q, " late ");
    q = put_dec(q, late_blocks);
    q = put_str(q, " missing ");
    q = put_dec(q, missing_blocks);
    q = put_str(q, " skipped ");
    q = put_dec(q, skipped);
    *q = 0;
    console_puts(text);
    console_puts("\n");
    last_frames = frames;
    last_bytes = bytes_in;
    last_report = now;
}

/* The state sector: 0, or -1. */
static int read_state(u8 *b, int ask_key)
{
    if (read_one(link_first + (ask_key ? AALINK_CTL_KEY : AALINK_CTL_STATE), b) || be32(b) != AALINK_CTL_MAGIC)
        return -1;
    return 0;
}

/* Where to start: the last key frame, with the codec's configuration
 * first; else wait for one (asked for). */
static int begin(void)
{
    static u8 b[512];
    u32 key, n;
    char text[64], *q;
    if (read_state(b, 0))
        return -1;
    key = be32(b + AALINK_CTL_O_KEY);
    n = be16(b + AALINK_CTL_O_CONFIG_LEN);
    head_hint = be32(b + AALINK_CTL_O_HEAD);
    q = put_str(text, "BRIDGE: ");
    q = put_str(q, be16(b + AALINK_CTL_O_FLAGS) & AALINK_PHONE ? "PHONE THERE" : "NO PHONE");
    q = put_str(q, be16(b + AALINK_CTL_O_FLAGS) & AALINK_VIDEO ? ", VIDEO" : ", NO VIDEO YET");
    *q = 0;
    line(text);
    if (n && n <= AALINK_CONFIG_MAX) {
        memcpy(messages, b + AALINK_CTL_CONFIG, n);
        decode(messages, n);
    }
    msg_used = msg_start = 0;
    in_message = skipping = 0;
    ndone = 0;
    if (key != AALINK_NONE) {
        next_seq = key;
        waiting_key = 0;
    } else {
        next_seq = be32(b + AALINK_CTL_O_TAIL);
        /* The video under way and its key frame gone: a new one. */
        waiting_key = (be16(b + AALINK_CTL_O_FLAGS) & AALINK_VIDEO) && be32(b + AALINK_CTL_O_MESSAGES) > 0;
        if (waiting_key)
            (void)read_state(b, 1);
    }
    return 0;
}

int main(void)
{
    struct aa_handoff *h = (struct aa_handoff *)AA_HANDOFF;
    u32 idle_since = 0;
    int r;

    board_init();
    console_puts("\nrnse sdk: aaviewer\n");
    screen_palette(BLACK, 0, 0, 0);
    screen_palette(WHITE, 255, 255, 255);
    screen_fill(0, 0, screen_width, screen_height, BLACK);
    if (screen_width != 400) {
        console_puts("the 192 only\n");
        return 1;
    }
    tables();
#ifdef AA_CRC
    crc_init();
#endif
    found_ccr = REG32(CCR);
    panel_attach();
    sd_idle = panel_service;
    display_idle = panel_service;
    loader_leave = leave;
    if (h->magic[0] == AA_MAGIC0 && h->magic[1] == AA_MAGIC1 && h->loader_return)
        loader_return_via = h->loader_return;
    line("RNSE SDK: AAVIEWER 1");
    line("HOLD RETURN 2 S: BACK TO THE LOADER");
    if (REG32(MMUCR) & 1) {
        line("ADDRESSES ARE TRANSLATED (MMUCR.AT): NOT RUN");
        for (;;)
            pause(1000);
    }
    /* The decoder's memory through P0; the caches as measured fastest
     * on a unit (examples/h264bench: copy-back, no index modes). */
    heap_init((void *)HEAP, HEAP_END - HEAP);
    h264bsdTimer = micros;
    h264bsdFastCopy = 1;
    dec = h264bsdAlloc();
    if (!dec || h264bsdInit(dec, 1) != 0) {
        line("NO MEMORY FOR THE DECODER");
        for (;;)
            pause(1000);
    }
    cache_setup(CCR_EMODE | CCR_ICE | CCR_OCE | CCR_CB);

    for (;;) {
        while (open_bridge()) {
            line("LOOKING AGAIN IN 3 S");
            pause(3000);
        }
        {
            char text[48], *q = put_str(text, "BRIDGE IN SLOT ");
            q = put_dec(q, (u32)slot + 1);
            q = put_str(q, ", AALINK.BIN AT SECTOR ");
            q = put_dec(q, link_first);
            *q = 0;
            line(text);
        }
        while (begin()) {
            line("THE BRIDGE'S STATE DID NOT READ: AGAIN");
            pause(1000);
            if (dead)
                break;
        }
        last_report = millis();
        while (!dead) {
            u32 want = (s32)(head_hint - next_seq) > 0 ? head_hint - next_seq : 1;
            r = read_blocks(want);
            if (r < 0)
                break;
            if (ndone) {
                /* The next read at once, a block, so that the bridge
                 * lets go of what has been read before this decodes. */
                (void)read_blocks(1);
                decode_done();
                idle_since = millis();
            } else if (r == 0) {
                /* Nothing new: a little later, and the state now and
                 * then (a phone gone, or a new one). */
                waits++;
                panel_service();
                if (millis() - idle_since > 2000) {
                    static u8 b[512];
                    if (read_state(b, 0) == 0) {
                        u32 tail = be32(b + AALINK_CTL_O_TAIL), head = be32(b + AALINK_CTL_O_HEAD);
                        /* Behind what the bridge keeps (a bridge that
                         * started anew): from the start again. */
                        if ((s32)(next_seq - tail) < 0 || (s32)(next_seq - head) > 0)
                            (void)begin();
                    }
                    idle_since = millis();
                }
                pause(2);
            }
            report();
        }
        line("THE CARD PROCESSOR STOPPED: STARTING AGAIN");
        dsp_reset();
        pause(500);
    }
}
