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
 * phone may go at the pace this decodes (a Pixel does not: it sends its
 * 30 a second regardless).  This reads all the bridge has between two
 * pictures, keeping the backlog in its own RAM, and decodes one picture
 * between two reads, showing only the newest, so that the sound keeps
 * coming.
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
 * On the 193 (8P0/8J0 035 193) the same program runs without a first
 * stage: the SD loader (rnse-sdk examples/sdload193) starts it from a
 * card as RNSE.BIN, its place being 1 MiB there.  The bridge is read
 * through the chip's own SD host (the kit's sdhi_read_multi: CMD18, some
 * 2.9 MB/s from the bridge on a unit), the picture is shown whole, 800
 * by 480, through the same palette, and the sound goes out through the
 * chip's serial sound interface (the kit's pcm_stream on the 193), not
 * through a card processor.  ON_193 below says which. */
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
/* On the 193: RAM from 0x08000000 to 0x0fffffff, the picture at
 * 0x08110000, the loader's place 0x09b60000 and the stock loader packed
 * after it up to some 0x09e40000: the decoder above all that. */
#define HEAP_193 0x0a000000
#define HEAP_END_193 0x0c000000

#define ON_193 (screen_width != 400)

#define RUN 64                       /* blocks a read at most: 32 KB */
/* The pictures read and not yet decoded, and their room: the phone
 * sends up to 30 a second whatever the acknowledgements say (a Pixel on
 * a unit, 2026-11-18), and the 192 decodes some 10 a second of a map
 * that moves, so they wait here, not in the bridge's small ring, which
 * then stays free for the sound.  Some 8 s of a moving map. */
#define MSG_ROOM (4u << 20)
#define DONE_MAX 256

/* The debug console's lines while the video runs (the counts every two
 * seconds, each input event): for the emulator's tests, built with
 * -DAA_CRC, only.  The port runs at 9600 bit/s and console_puts waits
 * for it: on a unit a report line held everything up for some 130 ms,
 * an input event's line for some 28 (2026-11-18). */
#if defined(AA_CRC) && !defined(AA_TRACE)
#define AA_TRACE
#endif

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

/* On the 193, records on CAN 0x7F8 for the bench, which has no
 * console (the debug port is not wired): the format of rnse-sdk's
 * bench programs (test, item, 32-bit value, 16-bit second), sent
 * through the stock software's set-up of the controller, mailbox 1, as
 * they do.  Nothing on the 192.
 *   0xd0 0  started; 0xd0 1 the bridge found: value its first sector,
 *           second the slot (1 = the unit's slot 1, 2 = slot 2)
 *   0xd3 s  slot s (as on the screen) did not do: value the code
 *   0xd4 0  long RETURN with no loader
 *   0xd5 n  every 2 s: n = 0 value frames, second fps x10; 1 value KB/s,
 *           second ms decoding a frame; 2 value ms showing, second ms
 *           reading; 3 value blocks read again, second missing; 4 value
 *           skipped pictures, second inputs sent
 *   0xd6 n  every 2 s, the sound: n = 0 value times the media ran dry,
 *           second gaps (pcm_gaps); 1 value media frames dropped,
 *           second the volume; 2 value the sound DMA channel's status,
 *           second the sound interface's status (its upper half); 3
 *           value what radio_source said (0 acknowledged, -9 not
 *           tried), second radio_answered; 4 value times the output
 *           stalled (took nothing for a second: the sound then gives
 *           way to the pictures), second 1 while it is stalled
 *   0xd9 s  whenever the display's state or its key changes: s the
 *           state (DISPLAY_*), value the position (0 open .. 1023
 *           closed), second 1 while the key is down
 *   0xd8 n  once, after the sound's start: the 193's audio registers
 *           (value), the address's low half (second)
 *   0xd7 n  every 2 s, the car (CAN): 0 value changes of the steering
 *           wheel's key seen, second frames lost (can_lost); 1 value
 *           car_ignition (0xffffffff: no ignition message seen), second
 *           the wheel's key now */
#define CAN193 0xffdf0000
#define CAN193_MB1 (CAN193 + 0x100 + 0x20)

static void say(u8 test, u8 item, u32 value, u32 second)
{
    u8 d[8] = { test, item, (u8)(value >> 24), (u8)(value >> 16), (u8)(value >> 8), (u8)value, (u8)(second >> 8), (u8)second };
    u32 n;
    int i;
    if (screen_width == 400)
        return;
    REG16(CAN193_MB1 + 0x00) = (u16)(0x7f8 << 2);
    for (i = 0; i < 8; i++)
        REG8(CAN193_MB1 + 0x08 + i) = d[i];
    REG16(CAN193_MB1 + 0x10) = 8;
    REG32(CAN193 + 0x20) = 0x00000002;
    for (n = 0; n < 400000; n++)
        if (REG16(CAN193 + 0x32) & 0x0002)
            break;
    if (REG16(CAN193 + 0x32) & 0x0002) {
        REG16(CAN193 + 0x32) = 0x0002;
        return;
    }
    REG32(CAN193 + 0x28) = 0x00000002;
    for (n = 0; n < 400000; n++)
        if (!(REG16(CAN193 + 0x22) & 0x0002) || (REG16(CAN193 + 0x3a) & 0x0002))
            break;
    REG16(CAN193 + 0x3a) = 0x0002;
}

/* Called while the program waits (main sets it: the sound fed). */
static void (*wait_idle)(void);

/* The 193's display and its key, reported on CAN whenever either
 * changes (0xd9: item the state, DISPLAY_*; value the position;
 * second the key, 1 down): what the bench needs when the key seems not
 * to answer.  Nothing on the 192. */
static void display_watch(void)
{
    static int key = -1, state = -1;
    static u32 at;
    int k, st;
    if (!ON_193 || millis() - at < 50)
        return;
    at = millis();
    k = panel_key(KEY_DISPLAY) != 0;
    st = display_state();
    if (k != key || st != state) {
        key = k;
        state = st;
        say(0xd9, (u8)st, (u32)display_position(), (u32)k);
    }
}

static void pause(u32 ms)
{
    u32 t = millis();
    while (millis() - t < ms) {
        panel_service();
        display_watch();
        if (wait_idle)
            wait_idle();
        cpu_idle();
    }
}

/* ---- colour --------------------------------------------------------- */

/* Colour by 5 bits of luma and 4 each of the two chroma samples, one
 * look-up a pixel (as examples/h264bench), into a palette of a 6x6x6
 * cube and 40 greys, each entry of the table the palette's nearest
 * colour.  The chroma steps are centred on 128 (each covers c - 8 to
 * c + 7 of 16 c), so that grey stays grey: with the middles of 16-wide
 * steps from 0 (h264bench's table) every grey came out 8 above 128 in
 * both, red and blue up and green down, the lilac and pink of the first
 * picture on a unit (2026-11-18).  The conversion is the stream's own:
 * its range (studio or full) and matrix (BT.601 or BT.709), from its
 * sequence parameter set when it has them, else BT.601 studio range. */
static u8 colour[32 * 16 * 16];
#define CHROMA(c) ((c) >= 248 ? 15 : ((c) + 8) >> 4)
#define PIXEL(y, cb, cr) colour[((y) >> 3) << 8 | CHROMA(cb) << 4 | CHROMA(cr)]
#define GREYS 40
static u8 grey_level[GREYS];
static int colour_range = -1, colour_matrix = -1;

static u32 clamp255(s32 v)
{
    return v < 0 ? 0 : v > 255 ? 255 : (u32)v;
}

/* The palette's nearest colour to r, g, b: the cube's (each level
 * rounded) or a grey's, whichever is nearer, weighted 2-4-3. */
static u8 nearest(u32 r, u32 g, u32 b)
{
    u32 cr = (r * 5 + 127) / 255, cg = (g * 5 + 127) / 255, cb = (b * 5 + 127) / 255;
    s32 dr = (s32)r - (s32)(cr * 51), dg = (s32)g - (s32)(cg * 51), db = (s32)b - (s32)(cb * 51);
    u32 best = (u32)(2 * dr * dr + 4 * dg * dg + 3 * db * db);
    u8 pick = (u8)(cr * 36 + cg * 6 + cb);
    /* The grey nearest the weighted lightness (they are evenly spaced),
     * then its distance. */
    u32 l = (2 * r + 4 * g + 3 * b) / 9, at = (l * (GREYS - 1) + 127) / 255;
    dr = (s32)r - grey_level[at];
    dg = (s32)g - grey_level[at];
    db = (s32)b - grey_level[at];
    if ((u32)(2 * dr * dr + 4 * dg * dg + 3 * db * db) < best)
        pick = (u8)(216 + at);
    return pick;
}

/* The table for a stream's range (1 full) and matrix (1 BT.709, else
 * BT.601), in 1/256. */
static void colour_table(int range, int matrix)
{
    s32 kr, kgb, kgr, kb, ky, off;
    u32 i;
    if (range == colour_range && matrix == colour_matrix)
        return;
    colour_range = range;
    colour_matrix = matrix;
    if (matrix == 1) {          /* BT.709 */
        kr = range ? 403 : 459, kgb = range ? 48 : 55, kgr = range ? 120 : 136, kb = range ? 475 : 541;
    } else {                    /* BT.601 */
        kr = range ? 359 : 409, kgb = range ? 88 : 100, kgr = range ? 183 : 208, kb = range ? 454 : 516;
    }
    ky = range ? 256 : 298;
    off = range ? 0 : 16;
    for (i = 0; i < sizeof colour; i++) {
        s32 y = (s32)(i >> 8) * 8 + 4, cb = (s32)((i >> 4) & 15) * 16 - 128, cr = (s32)(i & 15) * 16 - 128;
        s32 l = (y - off) * ky;
        colour[i] = nearest(clamp255((l + kr * cr) >> 8), clamp255((l - kgb * cb - kgr * cr) >> 8), clamp255((l + kb * cb) >> 8));
    }
}

static void tables(void)
{
    u32 k;
    for (k = 0; k < GREYS; k++)
        grey_level[k] = (u8)(k * 255 / (GREYS - 1));
    colour_table(0, 2);
}

/* The cube, then the greys (the last white, as the text before the
 * picture has it). */
static void palette(void)
{
    u32 i;
    for (i = 0; i < 216; i++)
        screen_palette((int)i, (u8)(i / 36 * 51), (u8)(i / 6 % 6 * 51), (u8)(i % 6 * 51));
    for (i = 0; i < GREYS; i++)
        screen_palette((int)(216 + i), grey_level[i], grey_level[i], grey_level[i]);
}

/* Called every 24 lines (main sets it: the faceplate, and the knob's
 * events). */
static void (*show_idle)(void) = panel_service;

/* The 193: the whole picture, a line at a time through the kit's
 * screen_blit, each pixel its own luma and its 2 x 2 block's chroma. */
static u8 show_line[800];

static void show_193(const u8 *pic, u32 w, u32 h)
{
    const u8 *cb = pic + w * h, *cr = cb + w * h / 4;
    u32 x, y, lines = h < (u32)screen_height ? h : (u32)screen_height;
    u32 cols = w < (u32)screen_width ? w : (u32)screen_width;
    if (cols > sizeof show_line)
        cols = sizeof show_line;
    for (y = 0; y < lines; y++) {
        const u8 *py = pic + y * w, *pb = cb + (y / 2) * (w / 2), *pr = cr + (y / 2) * (w / 2);
        if (!(y % 24))
            show_idle();
        for (x = 0; x < cols; x += 2) {
            show_line[x] = PIXEL(py[x], pb[x / 2], pr[x / 2]);
            show_line[x + 1] = PIXEL(py[x + 1], pb[x / 2], pr[x / 2]);
        }
        screen_blit(0, (int)y, (int)cols, 1, show_line, (int)cols);
    }
}

/* Every other pixel of every other line, as examples/h264bench shows. */
static void show(const u8 *pic, u32 w, u32 h)
{
    if (ON_193) {
        show_193(pic, w, h);
        return;
    }
    const u8 *cb = pic + w * h, *cr = cb + w * h / 4;
    u32 x, y, lines = (u32)screen_height < h / 2 ? (u32)screen_height : h / 2, cols = w / 2 > 400 ? 400 : w / 2;
    for (y = 0; y < lines; y++) {
        const u8 *py = pic + 2 * y * w, *pb = cb + y * (w / 2), *pr = cr + y * (w / 2);
        /* The faceplate every 24 lines, some 4 ms (see main). */
        if (!(y % 24))
            show_idle();
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
    if (ON_193)
        return; /* no card processor */
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
    if (ON_193) {
        /* The chip's SD host: blocks before a failure are good; the
         * failed one and those after it are read again. */
        int r = sdhi_read_multi(sector, n, to);
        if (r == -1 || r == -2 || r == -4)
            return r == -4 ? 0 : -1;
        for (i = 0; i < n; i++) {
            block_ok[i] = r == 0 || i < (u32)(-r - 0x100);
            if (!block_ok[i])
                missing_blocks++;
        }
        return 0;
    }
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
    if (ON_193)
        return sdhi_read(sector, buf) ? -1 : 0;
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

/* ---- sound ----------------------------------------------------------- */

/* The phone's sound (aalink.h): its samples as they come, a FIFO for
 * each of its sources, and the other card processor than the bridge's
 * playing a mix of them at 44.1 kHz (src/pcm.c: a ring of blocks fed by
 * DMA, which plays on while a picture is decoded), each source
 * resampled to it.  The phone's clock and the converter's are not the
 * same: each source's step is nudged by how full its FIFO is, so that it
 * stays some 300 ms ahead (enough for a key frame's decoding and showing
 * on a unit, 2026-11-18, where 200 ms ran dry now and then), a fraction
 * of a per cent of pitch.  A source
 * plays once it has that much, and stops (silence) when it runs dry.
 * The volume knob sets the amplifier's volume (radio_volume).
 *
 * The 193 plays the same mix through its own sound interface (the kit's
 * pcm_stream there; no card processor).  It takes no interrupt for it:
 * the ring moves on when sound_pump asks for a block, and the channel
 * holds two, so its blocks are twice as long (46 ms) for the same ring
 * (0.28 s), to ride out a picture's drawing between two pumps. */
#define SOUND_FRAMES (1024 * 12)      /* the ring: 0.28 s of stereo frames */
static u32 sound_block = 1024;        /* stereo frames a block (2048 on the 193) */
static s16 sound_ring[2 * SOUND_FRAMES] __attribute__((aligned(32)));
static int sound_dsp = -1;
static int volume = 12;
/* For the 193's reports (say 0xd6): what the amplifier answered. */
static int sound_radio = -9;
/* When the output last took a block, and how often it was found stuck:
 * an output that takes nothing for a second (the bench 193's first
 * run, 2026-11-21: its sound interface never started) must not hold
 * the pictures up behind a full FIFO, so the sound gives way then. */
static u32 sound_moved, sound_stalls;
static int sound_stalled;

struct source {
    s16 *buf;
    u32 frames, channels;            /* room, 1 or 2 */
    u32 in, out;                     /* frames, counting up */
    u32 frac;                        /* between out and out + 1, in 1/65536 */
    u32 base, step;                  /* its rate over 44.1 kHz, in 1/65536 */
    u32 target;                      /* frames to keep ahead */
    int playing;
    u32 dropped, dry;
};
#define MEDIA_FRAMES 65536            /* 1.4 s at 48 kHz */
#define VOICE_FRAMES 32768            /* 2 s at 16 kHz */
/* (From the heap, as messages[] is: see main.) */
static struct source sources[3] = {
    { 0, MEDIA_FRAMES, 2, 0, 0, 0, 71331, 71331, 14400, 0, 0, 0 },  /* 48000 / 44100, 300 ms */
    { 0, VOICE_FRAMES, 1, 0, 0, 0, 23777, 23777, 4800, 0, 0, 0 },   /* 16000 / 44100 */
    { 0, VOICE_FRAMES, 1, 0, 0, 0, 23777, 23777, 4800, 0, 0, 0 },
};

/* A block's samples (16-bit, little-endian) into its source's FIFO. */
static void sound_put(int type, const u8 *p, u32 n)
{
    struct source *f = &sources[type - AALINK_SOUND_MEDIA];
    u32 k, frames = n / (2 * f->channels);
    for (k = 0; k < frames; k++) {
        s16 *at;
        if (f->in - f->out >= f->frames - 1) {
            f->dropped += frames - k;
            return;
        }
        at = f->buf + (f->in % f->frames) * f->channels;
        at[0] = (s16)(p[0] | p[1] << 8);
        if (f->channels == 2)
            at[1] = (s16)(p[2] | p[3] << 8);
        p += 2 * f->channels;
        f->in++;
    }
}

/* The frames of sound the media FIFO has room for. */
static u32 sound_room(void)
{
    if (sound_dsp < 0)
        return 0x7fffffff; /* no output at all: nothing to wait for */
    if (millis() - sound_moved > 1000) {
        if (!sound_stalled)
            sound_stalls++;
        sound_stalled = 1;
        return 0x7fffffff; /* (sound_put drops what does not fit) */
    }
    sound_stalled = 0;
    return sources[0].frames - 1 - (sources[0].in - sources[0].out);
}

static s32 sat(s32 v)
{
    return v > 32767 ? 32767 : v < -32768 ? -32768 : v;
}

/* A block of the ring: each source's next frames, resampled, added. */
static void mix(s16 *out)
{
    u32 i, k;
    for (i = 0; i < 2 * sound_block; i++)
        out[i] = 0;
    for (k = 0; k < 3; k++) {
        struct source *f = &sources[k];
        u32 fill = f->in - f->out;
        s32 nudge;
        if (!f->playing) {
            if (fill < f->target)
                continue;
            f->playing = 1;
        }
        /* Ahead by more than target: a little faster, less: slower
         * (at most 1/200). */
        nudge = ((s32)fill - (s32)f->target) / 16;
        if (nudge > (s32)(f->base / 200))
            nudge = (s32)(f->base / 200);
        if (nudge < -(s32)(f->base / 200))
            nudge = -(s32)(f->base / 200);
        f->step = (u32)((s32)f->base + nudge);
        for (i = 0; i < sound_block; i++) {
            const s16 *a, *b;
            s32 l, r;
            if (f->in - f->out < 2) {
                f->playing = 0;
                f->dry++;
                break;
            }
            a = f->buf + (f->out % f->frames) * f->channels;
            b = f->buf + ((f->out + 1) % f->frames) * f->channels;
            /* (Fifteen bits of the fraction: a difference of up to
             * 65535 times it stays within 32 bits.) */
            l = a[0] + (((s32)b[0] - a[0]) * (s32)(f->frac >> 1) >> 15);
            r = f->channels == 2 ? a[1] + (((s32)b[1] - a[1]) * (s32)(f->frac >> 1) >> 15) : l;
            out[2 * i] = (s16)sat(out[2 * i] + l);
            out[2 * i + 1] = (s16)sat(out[2 * i + 1] + r);
            f->frac += f->step;
            f->out += f->frac >> 16;
            f->frac &= 0xffff;
        }
    }
}

/* The ring kept full: every block played is mixed again. */
static void sound_pump(void)
{
    s16 *b;
    if (sound_dsp < 0)
        return;
    while ((b = pcm_stream_block()) != 0) {
        mix(b);
        pcm_stream_ready();
        sound_moved = millis();
    }
}

/* The processor that is not the bridge's (slot ^ 1) takes the kit's
 * PCM program; the amplifier its sound.  (On the 193 the chip's own
 * sound interface, whatever dsp says.) */
static void sound_start(void)
{
    int dsp = slot ^ 1;
    u32 k;
    sound_block = ON_193 ? 2048 : 1024;
    for (k = 0; k < 3; k++) {
        sources[k].in = sources[k].out = sources[k].frac = 0;
        sources[k].playing = 0;
    }
    if (pcm_start(dsp)) {
        line(ON_193 ? "SOUND: NO OUTPUT" : "SOUND: THE OTHER CARD PROCESSOR DOES NOT START");
        return;
    }
    if (pcm_stream(sound_ring, sound_block, SOUND_FRAMES / sound_block)) {
        pcm_stop();
        line("SOUND: NO STREAM");
        return;
    }
    sound_dsp = dsp;
    sound_moved = millis();
    sound_radio = radio_source(RADIO_DECODER, volume);
    sound_pump();
    /* The 193's audio registers as they are now, for the bench (0xd8:
     * item, the register's value; second its address's low half). */
    if (ON_193) {
        static const u32 regs[] = { 0xff9900b8, 0xff973000, 0xff972000, 0xff986000, 0xff985000,
                                    0xff986010, 0xff909000, 0xff909004, 0xff984000, 0xff984004, 0xff8011b4,
                                    0xff93e008, 0xff90301c, 0xff900864 };
        u32 k;
        pause(50);
        for (k = 0; k < sizeof regs / sizeof regs[0]; k++)
            say(0xd8, (u8)k, regs[k] == 0xff973000 ? REG16(regs[k]) : REG32(regs[k]), regs[k] & 0xffff);
    }
}

static void sound_stop(void)
{
    if (sound_dsp >= 0)
        pcm_stop();
    sound_dsp = -1;
}

static void leave(void)
{
    sound_stop();
    sd_reader = 0;
    dsp_reset();
    if (!ON_193)
        cache_setup(found_ccr);
}

/* What a slot said, for the screen: SLOT n: then the words. */
static void slot_says(int s, const char *what, int r)
{
    say(0xd3, (u8)(s + 1), (u32)r, 0);
    char text[96], *q = put_str(text, "SLOT ");
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
    if (ON_193) {
        /* The chip's SD host, select 0 (the unit's slot 2) first: there
         * the bridge goes when this program is on a card in slot 1. */
        for (s = 0; s < 2; s++) {
            sd_reader = 0;
            r = sd_init(s);
            if (r) {
                slot_says(s == 0 ? 1 : 0, "NO CARD THAT STARTS", r);
                continue;
            }
            if (find_link()) {
                slot = s == 0 ? 1 : 0; /* the 192's numbering: 1 is SLOT 2 */
                return 0;
            }
            slot_says(s == 0 ? 1 : 0, "NO AALINK.BIN", 0);
        }
        line("NO BRIDGE (AALINK.BIN) IN EITHER SLOT");
        slot = -1;
        return -1;
    }
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
static u8 *messages;                 /* MSG_ROOM bytes from the heap: see main */
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
static u32 pictures, bytes_in, blocks_in, again, decode_us, show_us, read_us, waits, skipped, frames, sound_blocks;
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
    /* Sound: played as it comes, not gathered (its blocks come between
     * a picture's). */
    if (AALINK_IS_SOUND(AALINK_TYPE(flags))) {
        sound_put((int)AALINK_TYPE(flags), b + AALINK_HEADER, used);
        next_seq++;
        blocks_in++;
        bytes_in += used;
        sound_blocks++;
        return 1;
    }
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

/* ---- the keys and the knob, for the phone --------------------------- */

/* The unit's keys as Android Auto's (aalink.h, "Input"): the select knob
 * is the rotary controller, its push ENTER; RETURN pressed briefly is
 * BACK (held, the way back to the loader, as everywhere); NAV home, MEDIA
 * play/pause, NEXT and PREV the tracks, the four keys at the screen's
 * corners up, down, right and left.  The display key and CD EJECT stay
 * the unit's (panel_service).  The steering wheel's seek keys arrive as
 * NEXT and PREV and its volume keys as the volume knob (car_init). */
static const struct {
    u8 key, aa;
} key_map[] = {
    { KEY_SELECT, AALINK_KEY_ENTER }, { KEY_NAV, AALINK_KEY_HOME }, { KEY_MEDIA, AALINK_KEY_PLAY },
    { KEY_NEXT, AALINK_KEY_NEXT }, { KEY_PREV, AALINK_KEY_PREV },
    { KEY_SOFT_TL, AALINK_KEY_UP }, { KEY_SOFT_BL, AALINK_KEY_DOWN },
    { KEY_SOFT_TR, AALINK_KEY_RIGHT }, { KEY_SOFT_BR, AALINK_KEY_LEFT },
};
#define KEY_MAPS (sizeof key_map / sizeof key_map[0])
static u8 key_was[KEY_MAPS];
static u32 input_seq, inputs_sent;

/* An event to the bridge: the read of its input sector. */
static void send_input(u32 what)
{
    static u8 b[512];
    input_seq++;
    if (read_one(link_first + AALINK_INPUT_FIRST + AALINK_IN_CODE(input_seq, what), b) == 0)
        inputs_sent++;
#ifdef AA_TRACE
    console_puts("aaviewer: input ");
    console_hex(AALINK_IN_CODE(input_seq, what));
    console_puts("\n");
#endif
}

/* The pictures read and not yet decoded, said to the bridge at every
 * change and every half second (aalink.h, "backlog"). */
static void say_backlog(void)
{
    static u32 said = 0xffffffff, at;
    u32 n = ndone > 255 ? 255 : (u32)ndone;
    if (n == said && millis() - at < 500)
        return;
    said = n;
    at = millis();
    send_input(AALINK_IN_BACKLOG | n);
}

/* The car driver's other frames: the kit's canload's. */
static void car_frame_other(u16 id, const u8 *data, int len)
{
    (void)canload_frame(id, data, len);
}

/* Changes of the steering wheel's key (the 193's reports). */
static u32 wheel_changes;

static void inputs(void)
{
    u32 i;
    int v = panel_knob(KNOB_VOLUME), d = panel_knob(KNOB_SELECT);
    static int wheel;
    if (car_wheel() != wheel) {
        wheel = car_wheel();
        wheel_changes++;
    }
    /* The volume knob: the unit's amplifier, as everywhere. */
    if (v) {
        volume += v;
        volume = volume < 0 ? 0 : volume > 30 ? 30 : volume;
        if (sound_dsp >= 0)
            radio_volume(volume);
    }
    while (d) {
        int k = d > 100 ? 100 : d < -100 ? -100 : d;
        send_input(AALINK_IN_KNOB | ((u32)k & 0xff));
        d -= k;
    }
    for (i = 0; i < KEY_MAPS; i++) {
        int down = panel_key(key_map[i].key) != 0, pressed = panel_pressed(key_map[i].key);
        if (down != key_was[i]) {
            send_input((u32)key_map[i].aa << 1 | (u32)down);
            key_was[i] = (u8)down;
        } else if (pressed && !down) {
            /* Down and up between two looks. */
            send_input((u32)key_map[i].aa << 1 | 1);
            send_input((u32)key_map[i].aa << 1);
        }
    }
    if (panel_return_short()) {
        send_input(AALINK_KEY_BACK << 1 | 1);
        send_input(AALINK_KEY_BACK << 1);
    }
    /* Held with no loader to go back to (the 193's SD loader is on the
     * disc and starts this as an ordinary program): said, and the
     * viewer goes on, CD EJECT working as before. */
    if (panel_return_held()) {
        say(0xd4, 0, 0, 0);
        line("RETURN: NO LOADER TO GO BACK TO (CD EJECT, OR SWITCH OFF)");
    }
}

/* While a picture is decoded or shown: the faceplate served, and the
 * knob's and keys' events sent at once rather than after the picture
 * (some 100 ms later: the knob felt slow on a unit, 2026-11-18).  The
 * card processor is idle then, between two of its commands. */
static int link_up;

/* While a picture is decoded the card processor is idle: what the
 * bridge has is read meanwhile too, every 8 ms (a block every 20 ms to
 * see, when it had nothing), so that its small ring never fills while a
 * picture takes 100-200 ms to decode.  (It did, on a unit, 2026-11-18:
 * the bridge then held the phone's bytes back and dropped sound, and the
 * phone's sound stopped for want of its acknowledgements.)  What comes
 * is added after the messages waiting, so the one being decoded is not
 * touched. */
static u32 busy_read_at;
static int in_busy_read;

static void busy_read(void)
{
    u32 now = millis(), want = (s32)(head_hint - next_seq) > 0 ? head_hint - next_seq : 0;
    if (in_busy_read || now - busy_read_at < (want ? 8u : 20u))
        return;
    if (sound_room() < RUN * (AALINK_PAYLOAD / 4) || msg_used + RUN * 512 >= MSG_ROOM)
        return;
    in_busy_read = 1;
    busy_read_at = now;
    (void)read_blocks(want ? want : 1);
    in_busy_read = 0;
}

static void busy(void)
{
    panel_service();
    display_watch();
    sound_pump();
    if (link_up && !dead) {
        busy_read();
        inputs();
    }
}

/* ---- decoding ------------------------------------------------------ */

static storage_t *dec;
static u32 width, height;
#ifdef AA_CRC
static u32 crc;
#endif

/* draw: the pictures shown (else only decoded: a newer one waits). */
static void decode(u8 *p, u32 len, int draw)
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
        {
            static int said;
            u32 range = h264bsdVideoRange(dec), matrix = h264bsdMatrixCoefficients(dec);
            /* Said at the first picture and at every change.  (Its
             * text overran a buffer of 48 on a unit, 2026-11-18, where
             * the phone's stream changed the colours at once and so
             * said it: the stack spoilt, the unit reset.) */
            if (!said || (int)range != colour_range || (int)matrix != colour_matrix) {
                char text[80], *q = put_str(text, "aaviewer: colour: ");
                said = 1;
                q = put_str(q, range ? "full range, " : "studio range, ");
                q = put_str(q, matrix == 1 ? "BT.709" : "BT.601");
                q = put_str(q, " (matrix ");
                q = put_dec(q, matrix);
                *q++ = ')';
                *q = 0;
                console_puts(text);
                console_puts("\n");
                colour_table((int)range, (int)matrix);
            }
        }
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
            if (draw) {
                if (!showing) {
                    showing = 1;
                    palette();
                }
                show(pic, width, height);
            }
            frames++;
            t0 = micros();
            show_us += t0 - t1;
        }
    }
    decode_us += micros() - t0;
}

/* The first message read whole decoded (only one: reading again in
 * between keeps the sound coming, which a run of pictures decoded one
 * after the other, 100 ms each, would let run dry). */
/* Behind: the phone sends its 30 pictures a second whatever this does
 * (a Pixel with Apple Music beside Maps, on a unit, 2026-11-18: some 22
 * a second decoded, and 8 s behind within half a minute).  While more
 * than LAG_DROP wait, a picture that no later one is made of (its NAL
 * units' nal_ref_idc 0) is dropped undecoded.  The bridge, told the
 * backlog, holds the phone's acknowledgements (within the phone's
 * timeout), which slows the phone down.  (Not a key frame asked for to
 * start again from: the phone's video focus taken away and given back
 * makes its Android Auto rebuild its screen, often as its launcher: a
 * Pixel, 2026-11-18.) */
#define LAG_DROP 8
static u32 dropped_pictures;

/* Whether a picture is one no later picture refers to: its first VCL
 * NAL unit's nal_ref_idc is 0. */
static int unreferenced(const u8 *p, u32 n)
{
    u32 i;
    for (i = 0; i + 3 < n && i < 256; i++) {
        u32 t;
        if (p[i] || p[i + 1] || p[i + 2] != 1)
            continue;
        t = p[i + 3] & 0x1f;
        if (t == 1 || t == 5)
            return !(p[i + 3] & 0x60);
        i += 2;
    }
    return 0;
}

/* The first waiting message gone, undecoded or decoded. */
static void done_pop(void)
{
    int i;
    /* (Field by field: a structure's copy would be a library call the
     * kit does not have.) */
    for (i = 1; i < ndone; i++) {
        done[i - 1].at = done[i].at;
        done[i - 1].len = done[i].len;
        done[i - 1].type = done[i].type;
    }
    ndone--;
}

static void decode_done(void)
{
    int i;
    if (!ndone)
        return;
    {
        while (ndone > LAG_DROP && done[0].type == AALINK_PICTURE && unreferenced(messages + done[0].at, done[0].len)) {
            dropped_pictures++;
            done_pop();
        }
        if (ndone) {
            if (done[0].type == AALINK_PICTURE)
                pictures++;
            /* Shown only when no newer picture is waiting: every one has
             * to be decoded (the next is made of it), but showing one
             * that is replaced at once is 40 ms for nothing. */
            decode(messages + done[0].at, done[0].len, ndone == 1);
            done_pop();
        }
    }
    if (ndone)
        return;
    /* (Field by field: a structure's copy would be a library call the
     * kit does not have.) */
    (void)i;
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

#ifdef AA_TRACE
static void report(void)
{
    char text[256], *q = text;
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
    q = put_str(q, "; inputs ");
    q = put_dec(q, inputs_sent);
    q = put_str(q, "; dropped ");
    q = put_dec(q, dropped_pictures);
    q = put_str(q, "; sound blocks ");
    q = put_dec(q, sound_blocks);
    q = put_str(q, " dry ");
    q = put_dec(q, sources[0].dry + sources[1].dry + sources[2].dry);
    q = put_str(q, " dropped ");
    q = put_dec(q, sources[0].dropped + sources[1].dropped + sources[2].dropped);
    q = put_str(q, " gaps ");
    q = put_dec(q, pcm_gaps);
    q = put_str(q, "; wheel ");
    q = put_dec(q, wheel_changes);
    *q = 0;
    console_puts(text);
    console_puts("\n");
    last_frames = frames;
    last_bytes = bytes_in;
    last_report = now;
}
#else
/* On the 193 the numbers on CAN (say); nothing on the 192. */
static void report(void)
{
    u32 now = millis(), dt = now - last_report;
    static u32 last_frames, last_bytes;
    if (screen_width == 400 || dt < 2000)
        return;
    say(0xd5, 0, frames, (frames - last_frames) * 10000 / dt);
    say(0xd5, 1, (bytes_in - last_bytes) / dt, frames ? decode_us / 1000 / frames : 0);
    say(0xd5, 2, frames ? show_us / 1000 / frames : 0, frames ? read_us / 1000 / frames : 0);
    say(0xd5, 3, again, missing_blocks);
    say(0xd5, 4, skipped, inputs_sent);
    say(0xd6, 0, sources[0].dry, pcm_gaps);
    say(0xd6, 1, sources[0].dropped, (u32)volume);
    say(0xd6, 2, REG32(0xff8011b4), REG32(0xff984004) >> 16);
    say(0xd6, 3, (u32)sound_radio, (u32)radio_answered);
    say(0xd6, 4, sound_stalls, (u32)sound_stalled);
    say(0xd7, 0, wheel_changes, can_lost());
    say(0xd7, 1, (u32)car_ignition(), (u32)car_wheel());
    last_frames = frames;
    last_bytes = bytes_in;
    last_report = now;
}
#endif

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
        decode(messages, n, 1);
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
    say(0xd0, 0, 0, 0);
    screen_palette(BLACK, 0, 0, 0);
    screen_palette(WHITE, 255, 255, 255);
    screen_fill(0, 0, screen_width, screen_height, BLACK);
    tables();
#ifdef AA_CRC
    crc_init();
#endif
    found_ccr = REG32(CCR);
    /* The faceplate controller as aaview (and sdboot before it) left it,
     * lit; started otherwise (a test), it is started here. */
    if (!ON_193 && h->magic[0] == AA_MAGIC0 && h->magic[1] == AA_MAGIC1)
        panel_attach();
    else
        panel_init();
    /* On the 193 the faceplate's start is answered before any card is
     * touched: a viewer that went straight to an empty slot 2 (its
     * answers waited for up to half a second each) left a bench 193's
     * front dark for good (2026-11-21). */
    if (ON_193) {
        u32 t = millis();
        while (millis() - t < 600) {
            panel_service();
            cpu_idle();
        }
    }
    /* The car (the kit's src/car.c): served by panel_service, it puts
     * the steering wheel's keys among the faceplate's, so that seek
     * up/down are NEXT/PREV for the phone and the wheel's volume keys
     * turn the volume knob (inputs, below).  (The kit's car_init takes
     * the CAN controller as the stock software left it, can_attach: on
     * the 193 its can_init left the bus silent, and the viewer's
     * reports, say, go through the controller as it was left.) */
    car_init();
    /* A new viewer over CAN (the kit's canload, from canbench --push):
     * on the 193 into the 1 MiB above the heap, where the SD loader
     * also loads a program; the frames the car driver does not use come
     * here.  Not on the 192 (its RAM is laid out otherwise). */
    if (ON_193) {
        canload_init((void *)0x8c000000, 0x100000);
        car_other = car_frame_other;
    }
    sd_idle = panel_service;
    display_idle = panel_service;
    loader_leave = leave;
    if (!ON_193 && h->magic[0] == AA_MAGIC0 && h->magic[1] == AA_MAGIC1 && h->loader_return)
        loader_return_via = h->loader_return;
    line("RNSE SDK: AAVIEWER 1");
    line(loader_active() ? "HOLD RETURN 2 S: BACK TO THE LOADER" : "CD EJECT WORKS; NO LOADER TO RETURN TO");
    if (REG32(MMUCR) & 1) {
        line("ADDRESSES ARE TRANSLATED (MMUCR.AT): NOT RUN");
        for (;;)
            pause(1000);
    }
    /* The decoder's memory through P0; the caches as measured fastest
     * on a unit (examples/h264bench: copy-back, no index modes). */
    if (ON_193)
        heap_init((void *)HEAP_193, HEAP_END_193 - HEAP_193);
    else
        heap_init((void *)HEAP, HEAP_END - HEAP);
    h264bsdTimer = micros;
    h264bsdFastCopy = 1;
    /* The faceplate served every few milliseconds while a picture is
     * decoded too: its line (9600 bit/s) has only a few bytes of buffer,
     * and once a picture, some 100 ms, lost key frames (on a unit,
     * 2026-11-18, the keys seemed dead). */
    h264bsdIdle = busy;
    show_idle = busy;
    wait_idle = sound_pump;
    /* The big buffers from the heap, not .bss: the start-up code clears
     * .bss a word at a time before the tick (and with it the watchdog's
     * strobe) runs, and 4.5 MB of it outlasted the watchdog's 105 ms on
     * a unit (2026-11-18: reset to the stock software at once). */
    messages = malloc(MSG_ROOM);
    sources[0].buf = malloc(2 * MEDIA_FRAMES * sizeof(s16));
    sources[1].buf = malloc(VOICE_FRAMES * sizeof(s16));
    sources[2].buf = malloc(VOICE_FRAMES * sizeof(s16));
    dec = h264bsdAlloc();
    if (!messages || !sources[0].buf || !sources[1].buf || !sources[2].buf || !dec || h264bsdInit(dec, 1) != 0) {
        line("NO MEMORY FOR THE DECODER");
        for (;;)
            pause(1000);
    }
    /* (The 193's SH-4A keeps its caches as its software left them.) */
    if (!ON_193)
        cache_setup(CCR_EMODE | CCR_ICE | CCR_OCE | CCR_CB);

    for (;;) {
        while (open_bridge()) {
            line("LOOKING AGAIN IN 3 S");
            pause(3000);
        }
        say(0xd0, 1, link_first, (u32)slot + 1);
        {
            char text[96], *q = put_str(text, "BRIDGE IN SLOT ");
            q = put_dec(q, (u32)slot + 1);
            q = put_str(q, ", AALINK.BIN AT SECTOR ");
            q = put_dec(q, link_first);
            *q = 0;
            line(text);
        }
        /* (From the clock: a viewer started again does not begin with
         * the code the bridge saw last, which it would take for a read
         * made again.) */
        input_seq = millis();
        while (begin()) {
            line("THE BRIDGE'S STATE DID NOT READ: AGAIN");
            pause(1000);
            if (dead)
                break;
        }
        sound_start();
        last_report = millis();
        link_up = 1;
        while (!dead) {
            u32 want = (s32)(head_hint - next_seq) > 0 ? head_hint - next_seq : 1;
            sound_pump();
            /* No more read while the sound has no room for a run of it
             * (the phone sends it as it plays: only a bridge that has
             * kept much of it gives so much at once). */
            if (sound_room() < RUN * (AALINK_PAYLOAD / 4)) {
                pause(5);
                inputs();
                report(); /* (the reports go on while it waits) */
                continue;
            }
            /* All the bridge has (up to 256 KB, some 130 ms at 2 MB/s),
             * run after run, before the next picture is decoded: its
             * ring emptied every round, so that the sound never waits
             * behind pictures there (on a unit, 2026-11-18, it was
             * dropped while the 192 decoded a moving map). */
            {
                u32 got = 0;
                do {
                    r = read_blocks(want);
                    if (r <= 0)
                        break;
                    got += (u32)r;
                    want = (s32)(head_hint - next_seq) > 0 ? head_hint - next_seq : 0;
                    sound_pump();
                } while (want && got < 512 && sound_room() >= RUN * (AALINK_PAYLOAD / 4) && msg_used + RUN * 512 < MSG_ROOM);
                if (r > 0 || got)
                    r = (int)got;
            }
            if (r < 0)
                break;
            say_backlog();
            if (ndone) {
                /* On the 193 the SD reads gulp a big backlog each round
                 * (fast reads, and no sound to pace them), so decoding
                 * one picture a round never lets the queue fall to one,
                 * and decode_done draws only the newest (ndone == 1):
                 * the phone's video stayed black though every frame was
                 * decoded (the bench, 2026-11-21).  Drain it here, which
                 * decodes all and draws the last.  The 192 keeps one a
                 * round, for the sound. */
                if (ON_193)
                    while (ndone && !dead)
                        decode_done();
                else
                    decode_done();
                say_backlog();
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
            inputs();
            report();
        }
        link_up = 0;
        /* (open_bridge may look at the other slot: its processor is
         * the sound's.) */
        sound_stop();
        line("THE CARD PROCESSOR STOPPED: STARTING AGAIN");
        dsp_reset();
        pause(500);
    }
}
