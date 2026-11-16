/* How fast bytes get from an SD slot into the 192's main memory: the
 * link an Android Auto bridge in a card slot would have (a
 * microcontroller that is the phone's USB host on one side and answers
 * as an SD card on the other; docs/android-auto.md).  Started from an SD
 * card by examples/sdboot (LINKBENC.BIN, say).
 *
 * The stock way, the installed software's program in the card
 * processor and a message for every sector, gave 297 KB/s on a unit.
 * This loads a program of its own into the card's processor (NEC
 * uPD77115A, the table `program` below, written out by hand as in
 * examples/report) that runs every card command itself, the start-up
 * included, and moves data without a message per sector:
 *
 *   - sectors one command each (CMD17), the processor's buffer transfer
 *     unit filling its memory, then the words to the host;
 *   - sectors by one command (CMD18) in runs of 64, two buffers in
 *     turn, so that the next block comes in while one goes out; a block
 *     whose words had begun to arrive before its buffer was ready is
 *     counted ("late"), as is a block that did not come in time;
 *   - the host port alone, both ways, with no card: a count from the
 *     processor, and words to it added up.
 *
 * The main processor takes the words through the host port, a word at
 * a time with the "nothing to read" pin looked at before each (POLLED),
 * or, after a block's first word, the 256 of the block without looking
 * (BURST: whether the port keeps up with that is one of the questions).
 * The card interface's clock is the stock setting (the processor's
 * clock / 4, 16.9 MHz if that is 67.7 MHz), / 3, and / 2, which is over
 * the 25 MHz of a card's default speed if the clock is that.
 *
 * What is read is checked: the card should have LINKTEST.BIN in its top
 * directory (tools/linktest.py writes one, 2 MB or more): every 32-bit
 * word of it is a function of its place (`pattern` below), so a word
 * that is lost, repeated or from another place shows.  The file's first
 * contiguous run of clusters, up to 2 MB, is what is read.  Without the
 * file, the card's first 2 MB are read and compared with what CMD17
 * read of them.  The host port tests check their own words.
 *
 * Before it takes the processor over, the program asks the installed
 * software's program (V222 0371 only, through src/sd.c) for the
 * processor's port pins and copies them into its own: what pins 0 and
 * 1 drive is not known, and the card may need it.  On a unit with other
 * software the pins stay inputs, as the reset leaves them.  With the
 * stock program it also times the stock way over the same sectors, for
 * comparison.
 *
 * Nothing is written to the card.  Results on the screen and as records
 * on CAN 0x7F8 (test, item, a value of 32 bits and one of 16), as
 * examples/h264bench sends them:
 *
 *   0x5f  the run: slot and what the stock start said (0); the copied
 *         port pins, data and direction (1); the card's start, 0 or the
 *         step that failed, and the OCR (2); the first sector read and
 *         how many (3); 1 = LINKTEST.BIN, 0 = the card's start, and the
 *         card's addressing (1 sectors, 0 bytes) (4)
 *   0x60 + n: test n (the table `tests`): bytes, and 1 ran + 2 failed
 *         + 4 checked (0); bad sectors and blocks that did not come in
 *         time (1); late blocks and the card interface's status bits
 *         seen (2); KB/s and Mbit/s times ten (3); microseconds (4)
 *   0xff  done.
 *
 * The keys are answered between runs: the display key and CD EJECT work
 * all the while, and RETURN held for two seconds returns to the SD
 * loader (the card processor is put back into reset first; the loader
 * starts it anew).
 *
 * The 192 only. */
#include "rnse.h"

#define STATUS 0xb2400001 /* bits 2, 0: nothing to read; 3, 1: cannot take a word */
#define LATCH 0xb6000004  /* bits 2, 3: the processors' reset lines, low = reset */
static const u32 ports[2] = { 0xb5000002, 0xb500004a };

/* Memory, above the way back's scratch and the loader session's place. */
#define DATA ((u8 *)0x8c800000)          /* 2 MB: what a test read */
#define REF ((u8 *)0x8ca00000)           /* 2 MB: without LINKTEST.BIN */
#define CHAIN ((u32 *)0x8cc00000)        /* the file's clusters */
#define CHAIN_MAX 65536
#define MAX_SECTORS 4096                 /* 2 MB */
#define RUN 64                           /* blocks a command (CMD18) or a message */

#define WHITE 1
#define BLACK 0

/* The program for the card processor: instruction words from 0x200; see
 * src/dev/upd77115.rs for the encodings.  Messages from the host, a
 * word each, every one answered:
 *
 *   1             ping: a5a5
 *   2 a v         X memory a = v (a register, 0x3800 on): f000
 *   3 v           the card interface's setting (SDCTL, its clock): f000
 *   4 a           X memory a: its value
 *   5 c h l       a card command, c the command register's value, h:l
 *                 the argument: SDCTL (| 0x80 if no response came) and
 *                 the response register's eight words
 *   6 h l H L n   n sectors from address h:l, the address advanced by
 *                 H:L each (CMD17): per sector a header (SDCTL | 0x80
 *                 when the response or the block did not come) and 256
 *                 words; then f000
 *   7 h l n       n sectors from h:l by CMD18: per sector a header and
 *                 256 words, then CMD12: SDCTL after it and the number
 *                 of late blocks
 *   8 n           n blocks of 5a5a and 256 words of a count: f000
 *   9 n           n * 256 words to the processor: their sum, and f000
 *
 * It says 1b01 when it has started. */
static const u32 program[] = {
    /* 0x200: the reset vector, then fifteen more of four words each,
     * every one a return from interrupt (all are masked). */
    0x2c002000, 0, 0, 0,          /* jmp 0x240 */
    0x26000000, 0, 0, 0, 0x26000000, 0, 0, 0, 0x26000000, 0, 0, 0,
    0x26000000, 0, 0, 0, 0x26000000, 0, 0, 0, 0x26000000, 0, 0, 0,
    0x26000000, 0, 0, 0, 0x26000000, 0, 0, 0, 0x26000000, 0, 0, 0,
    0x26000000, 0, 0, 0, 0x26000000, 0, 0, 0, 0x26000000, 0, 0, 0,
    0x26000000, 0, 0, 0, 0x26000000, 0, 0, 0, 0x26000000, 0, 0, 0,
    /* 0x240 */
    0x61000000,     /* r0 = 0 */
    0x61040000,     /* r1 = 0 */
    0x61080000,     /* r2 = 0 */
    0x610c0000,     /* r3 = 0 */
    0x61100000,     /* r4 = 0 */
    0x61140000,     /* r5 = 0 */
    0x61180000,     /* r6 = 0 */
    0x611c0000,     /* r7 = 0 */
    0x38400fff,     /* sr = 0x0fff: every interrupt masked */
    0x38810100,     /* r1l = 0x100 */
    0x4890382e,     /* clock control = r1l: the multiplier on (as the stock program and examples/report) */
    0x1c004e20,     /* rep 20000 */
    0x00000000,     /* nop: while it settles */
    0x38810180,     /* r1l = 0x180 */
    0x4890382e,     /* clock control = r1l: and the clock from it */
    0x00000000,     /* nop */
    0x00000000,     /* nop */
    0x61040000,     /* r1 = 0 */
    0x3b813802,     /* r7l = 0x3802: the card interface's setting (slow clock) */
    0x38011b01,     /* r0l = 0x1b01: hello, this program's version */
    0x48103806,     /* hdt = r0l */
    /* 0x0255 main */
    0x61000000,     /* r0 = 0 */
    0x48113806,     /* r0l = hdt: what to do */
    0x59040001,     /* r1 = r0 - 1 */
    0x2c000a11,     /* if r1 = 0: ping */
    0x59040002,     /* r1 = r0 - 2 */
    0x2c000a91,     /* if r1 = 0: wrreg */
    0x59040003,     /* r1 = r0 - 3 */
    0x2c000d91,     /* if r1 = 0: setctl */
    0x59040004,     /* r1 = r0 - 4 */
    0x2c000f91,     /* if r1 = 0: rdreg */
    0x59040005,     /* r1 = r0 - 5 */
    0x2c001211,     /* if r1 = 0: cmd */
    0x59040006,     /* r1 = r0 - 6 */
    0x2c003511,     /* if r1 = 0: read1 */
    0x59040007,     /* r1 = r0 - 7 */
    0x2c004091,     /* if r1 = 0: readm */
    0x59040008,     /* r1 = r0 - 8 */
    0x2c005211,     /* if r1 = 0: portout */
    0x59040009,     /* r1 = r0 - 9 */
    0x2c005911,     /* if r1 = 0: portin */
    0x3881eeee,     /* r1l = 0xeeee: not known */
    0x48903806,     /* hdt = r1l */
    0x2c7ff500,     /* jmp main */
    /* 0x026c ping */
    0x3881a5a5,     /* r1l = 0xa5a5 */
    0x48903806,     /* hdt = r1l */
    0x2c7ff380,     /* jmp main */
    /* 0x026f wrreg */
    0x61040000,     /* r1 = 0 */
    0x48913806,     /* r1l = hdt: an address of X memory */
    0x3c800000,     /* dp0 = r1l */
    0x48913806,     /* r1l = hdt: the value */
    0x44900000,     /* *dp0 = r1l */
    0x3881f000,     /* r1l = 0xf000: done */
    0x48903806,     /* hdt = r1l */
    0x2c7fef80,     /* jmp main */
    /* 0x0277 setctl */
    0x611c0000,     /* r7 = 0 */
    0x4b913806,     /* r7l = hdt: the card interface's setting */
    0x4b903814,     /* sdctl = r7l (and its error flags cleared) */
    0x3881f000,     /* r1l = 0xf000: done */
    0x48903806,     /* hdt = r1l */
    0x2c7fec80,     /* jmp main */
    /* 0x027d rdreg */
    0x61040000,     /* r1 = 0 */
    0x48913806,     /* r1l = hdt: an address of X memory */
    0x3c800000,     /* dp0 = r1l */
    0x00000000,     /* nop */
    0x44910000,     /* r1l = *dp0 */
    0x48903806,     /* hdt = r1l */
    0x2c7fe900,     /* jmp main */
    /* 0x0284 cmd */
    0x61040000,     /* r1 = 0 */
    0x48913806,     /* r1l = hdt: the command register's value */
    0x48903811,     /* sdidx = r1l */
    0x48113806,     /* r0l = hdt: the argument, high half */
    0x48103812,     /* agh = r0l */
    0x48113806,     /* r0l = hdt: low half */
    0x48103813,     /* agl = r0l */
    0x2e000580,     /* call issue */
    0x48113814,     /* r0l = sdctl */
    0x8bc00000,     /* r0 = r0 | r6: and 0x80 for a timeout */
    0x48103806,     /* hdt = r0l */
    0x38003815,     /* dp0 = rpr */
    0x38083806,     /* dp4 = hdt */
    0x00000000,     /* nop */
    0x70020800,     /* r0 = *dp0: the first word of the response */
    0x1c000008,     /* rep 8 */
    0x70020808,     /* *dp4 = r0h; r0 = *dp0: and the rest, 8 in all */
    0x2c7fe000,     /* jmp main */
    /* 0x0296 issue */
    0x5bf84000,     /* r6 = r7 | 0x4000 */
    0x4b103814,     /* sdctl = r6l: the command goes out */
    0x5a200400,     /* r0 = r1 & 0x400: a response? */
    0x61180000,     /* r6 = 0 */
    0x3b01ffff,     /* r6l = 0xffff: the time allowed, 37 clocks a round */
    0x2c000510,     /* if r0 = 0: nores */
    /* 0x029c resp */
    0x48113814,     /* r0l = sdctl */
    0x5a000004,     /* r0 = r0 & 4: the response is in */
    0x2c000818,     /* if r0 != 0: came */
    0x1c000020,     /* rep 32 */
    0x00000000,     /* nop */
    0x63d80000,     /* r6 = r6 - 1 */
    0x2c7ffd1e,     /* if r6 != 0: resp */
    0x3b010080,     /* r6l = 0x80: none came */
    0x24000000,     /* ret */
    /* 0x02a5 nores */
    0x48113814,     /* r0l = sdctl */
    0x5a004000,     /* r0 = r0 & 0x4000: the command still on the line */
    0x2c000390,     /* if r0 = 0: came */
    0x1c000020,     /* rep 32 */
    0x00000000,     /* nop */
    0x63d80000,     /* r6 = r6 - 1 */
    0x2c7ffd1e,     /* if r6 != 0: nores */
    0x3b010080,     /* r6l = 0x80 */
    0x24000000,     /* ret */
    /* 0x02ae came */
    0x61180000,     /* r6 = 0 */
    0x24000000,     /* ret */
    /* 0x02b0 arm */
    0x38010000,     /* r0l = 0 */
    0x48103833,     /* pcr = r0l: stopped */
    0x5a800fff,     /* r0 = r4 & 0xfff */
    0x48103830,     /* psar = r0l */
    0x38010100,     /* r0l = 0x100 */
    0x48103831,     /* psr = r0l: 256 words */
    0x38010002,     /* r0l = 2 */
    0x48103833,     /* pcr = r0l: started, into X memory */
    0x24000000,     /* ret */
    /* 0x02b9 filled */
    0x61040000,     /* r1 = 0 */
    0x3881ffff,     /* r1l = 0xffff */
    /* 0x02bb fl */
    0x48113833,     /* r0l = pcr */
    0x5a000002,     /* r0 = r0 & 2: still running */
    0x2c000310,     /* if r0 = 0: flok */
    0x1c000020,     /* rep 32 */
    0x00000000,     /* nop */
    0x63240000,     /* r1 = r1 - 1 */
    0x2c7ffd19,     /* if r1 != 0: fl */
    0x5bd80080,     /* r6 = r6 | 0x80: they did not come */
    /* 0x02c3 flok */
    0x24000000,     /* ret */
    /* 0x02c4 send */
    0x48913814,     /* r1l = sdctl */
    0x9bc40000,     /* r1 = r1 | r6 */
    0x38083806,     /* dp4 = hdt */
    0x70021000,     /* r0 = *dp0++ */
    0x48903806,     /* hdt = r1l: the header, and at once the words */
    0x1c000100,     /* rep 256 */
    0x70021008,     /* *dp4 = r0h; r0 = *dp0++ */
    0x24000000,     /* ret */
    /* 0x02cc read1 */
    0x61080000,     /* r2 = 0 */
    0x49213806,     /* r2h = hdt: the address */
    0x49113806,     /* r2l = hdt */
    0x610c0000,     /* r3 = 0 */
    0x49a13806,     /* r3h = hdt: the step */
    0x49913806,     /* r3l = hdt */
    0x61140000,     /* r5 = 0 */
    0x4a913806,     /* r5l = hdt: how many */
    0x61100000,     /* r4 = 0 */
    0x3a015000,     /* r4l = 0x5000: the buffer */
    /* 0x02d6 r1blk */
    0x2e7fed00,     /* call arm */
    0x38814d11,     /* r1l = 0x4d11: CMD17, a response and a block */
    0x48903811,     /* sdidx = r1l */
    0x49203812,     /* agh = r2h */
    0x49103813,     /* agl = r2l */
    0x2e7fdd80,     /* call issue */
    0x2e7fee80,     /* call filled */
    0x38005000,     /* dp0 = 0x5000 */
    0x2e7ff300,     /* call send */
    0xa8680000,     /* r2 = r2 + r3: the next address */
    0x63b40000,     /* r5 = r5 - 1 */
    0x2c7ffa9d,     /* if r5 != 0: r1blk */
    0x3881f000,     /* r1l = 0xf000: done */
    0x48903806,     /* hdt = r1l */
    0x2c7fb880,     /* jmp main */
    /* 0x02e5 readm */
    0x61080000,     /* r2 = 0 */
    0x49213806,     /* r2h = hdt: the address */
    0x49113806,     /* r2l = hdt */
    0x61140000,     /* r5 = 0 */
    0x4a913806,     /* r5l = hdt: how many */
    0x610c0000,     /* r3 = 0: late blocks */
    0x61100000,     /* r4 = 0 */
    0x3a015000,     /* r4l = 0x5000 */
    0x2e7fe180,     /* call arm */
    0x38814d12,     /* r1l = 0x4d12: CMD18 */
    0x48903811,     /* sdidx = r1l */
    0x49203812,     /* agh = r2h */
    0x49103813,     /* agl = r2l */
    0x2e7fd200,     /* call issue */
    /* 0x02f3 rmblk */
    0x2e7fe300,     /* call filled */
    0x3e000000,     /* dp0 = r4l: this block */
    0x5c900100,     /* r4 = r4 ^ 0x100: the other buffer */
    0x63b40000,     /* r5 = r5 - 1 */
    0x2c000295,     /* if r5 = 0: rmsend */
    0x48113814,     /* r0l = sdctl */
    0x5a000001,     /* r0 = r0 & 1: words of the next block already there */
    0x626c0018,     /* if r0 != 0: r3 = r3 + 1 */
    0x2e7fda80,     /* call arm: for the next block */
    /* 0x02fc rmsend */
    0x2e7fe400,     /* call send */
    0x61180000,     /* r6 = 0 */
    0x2c7ffa9d,     /* if r5 != 0: rmblk */
    0x38814c0c,     /* r1l = 0x4c0c: CMD12, a response */
    0x48903811,     /* sdidx = r1l */
    0x38010000,     /* r0l = 0 */
    0x48103812,     /* agh = r0l */
    0x48103813,     /* agl = r0l */
    0x2e7fc900,     /* call issue */
    0x48113814,     /* r0l = sdctl */
    0x8bc00000,     /* r0 = r0 | r6 */
    0x48103806,     /* hdt = r0l */
    0x49903806,     /* hdt = r3l: late blocks */
    0x2c7fa600,     /* jmp main */
    /* 0x030a portout */
    0x61140000,     /* r5 = 0 */
    0x4a913806,     /* r5l = hdt: how many */
    0x61040000,     /* r1 = 0: the count, in r1h */
    0x610c0000,     /* r3 = 0 */
    0x39810001,     /* r3l = 1 */
    0x5f6c0010,     /* r3 = r3 << 16: 1 in r3h */
    0x38083806,     /* dp4 = hdt */
    /* 0x0311 poblk */
    0x38015a5a,     /* r0l = 0x5a5a */
    0x48103806,     /* hdt = r0l: the header */
    0x1c000100,     /* rep 256 */
    0x98640009,     /* *dp4 = r1h; r1 = r1 + r3 */
    0x63b40000,     /* r5 = r5 - 1 */
    0x2c7ffd9d,     /* if r5 != 0: poblk */
    0x3881f000,     /* r1l = 0xf000: done */
    0x48903806,     /* hdt = r1l */
    0x2c7f9e00,     /* jmp main */
    /* 0x031a portin */
    0x61140000,     /* r5 = 0 */
    0x4a913806,     /* r5l = hdt: how many */
    0x61000000,     /* r0 = 0 */
    0x61080000,     /* r2 = 0 */
    0x38083806,     /* dp4 = hdt */
    /* 0x031f piblk */
    0x1c000100,     /* rep 256 */
    0xa8090008,     /* r2 = r2 + r0; r0 = *dp4 */
    0x63b40000,     /* r5 = r5 - 1 */
    0x2c7ffe9d,     /* if r5 != 0: piblk */
    0xa8080000,     /* r2 = r2 + r0: the last word */
    0x49203806,     /* hdt = r2h: the sum */
    0x3881f000,     /* r1l = 0xf000: done */
    0x48903806,     /* hdt = r1l */
    0x2c7f9700,     /* jmp main */
};

/* The card interface's setting: its clock, the processor's divided by
 * the divisor of index i (1, 2, 3, 4, 5, 7, 8, 9, 10, 12, 16, 32, 64,
 * 128, 256, 512), as src/dev/upd77115.rs has the field.  The stock
 * driver's: 0x3802 (/ 256) for the card's start, 0x0c02 (/ 4) after. */
#define CTL(i) ((u16)((i) << 10 | 2))
#define CTL_SLOW CTL(14)
#define CTL_FAST CTL(3)

enum { STOCK, PORT_OUT, PORT_IN, CARD };
static const struct test {
    const char *name;
    u8 kind, multi, burst;
    u16 ctl;
} tests[] = {
    { "STOCK DRIVER", STOCK, 0, 0, 0 },
    { "PORT OUT", PORT_OUT, 0, 0, 0 },
    { "PORT IN", PORT_IN, 0, 0, 0 },
    { "CMD17 /4", CARD, 0, 0, CTL(3) },
    { "CMD18 /4", CARD, 1, 0, CTL(3) },
    { "CMD18 /3", CARD, 1, 0, CTL(2) },
    { "PORT OUT BURST", PORT_OUT, 0, 1, 0 },
    { "CMD18 /4 BURST", CARD, 1, 1, CTL(3) },
    { "CMD18 /3 BURST", CARD, 1, 1, CTL(2) },
    { "CMD18 /2", CARD, 1, 0, CTL(1) },
    { "CMD18 /2 BURST", CARD, 1, 1, CTL(1) },
};
#define TESTS (int)(sizeof tests / sizeof tests[0])

static struct result {
    u32 bytes, us, bad, timeouts, late;
    u16 status;
    u8 ran, failed, checked;
} res[TESTS];

static int slot = -1;
static u32 port;
static u8 none_bit, full_bit;
static int dead;                 /* the processor stopped answering */
static int card_ok, high_capacity, have_file, have_ref;
static u32 ocr, first_sector, sectors;
static u16 pins_data, pins_dir;
static int pins_known;

static int row;

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

static char *put_hex(char *p, u32 v, int digits)
{
    int i;
    for (i = digits - 1; i >= 0; i--)
        *p++ = "0123456789ABCDEF"[(v >> (4 * i)) & 15];
    return p;
}

static void line(const char *text)
{
    int y = 2 + 10 * row;
    screen_fill(0, y - 1, screen_width, 10, BLACK);
    screen_text(2, y, text, WHITE, 1);
    console_puts(text);
    console_puts("\n");
    if (++row >= 23)
        row = 0;
}

static void rec(u8 test, u8 item, u32 value, u32 second)
{
    u8 d[8] = { test, item, (u8)(value >> 24), (u8)(value >> 16), (u8)(value >> 8), (u8)value, (u8)(second >> 8), (u8)second };
    can_send_for(0x7f8, d, 8, 3000);
}

static void pause(u32 ms)
{
    u32 t = millis();
    while (millis() - t < ms) {
        panel_service();
        cpu_idle();
    }
}

/* ---- the host port ------------------------------------------------- */

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

/* The program into the processor, as examples/report loads its own:
 * the boot ROM's download, then its hello and a ping. */
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

/* A card command: the header (SDCTL as the processor saw it after the
 * response, 0x80 set when none came), or -1; the response's words in r. */
static int card_cmd(u16 reg, u32 arg, u16 *r)
{
    u16 h;
    int i;
    if (put(5) || put(reg) || put((u16)(arg >> 16)) || put((u16)arg) || get(&h, 1000))
        return -1;
    for (i = 0; i < 8; i++)
        if (get(&r[i], 100))
            return -1;
    return h;
}

static int responded(int h)
{
    return h >= 0 && !(h & 0x80);
}

/* The 32 bits of a short response. */
static u32 r32(const u16 *r)
{
    return (u32)(r[0] & 0xff) << 24 | (u32)r[1] << 8 | r[2] >> 8;
}

/* The card's start, as src/sd.c has the stock driver do it (SD cards
 * of both kinds; no MultiMediaCard): 0, or the step that failed. */
static int card_start(void)
{
    u16 r[8];
    u32 hcs, t, rca;
    int h;
    if (setctl(CTL_SLOW))
        return 1;
    pause(20);
    if (card_cmd(0x4800, 0, r) < 0)                          /* CMD0 */
        return 2;
    pause(20);
    h = card_cmd(0x4c08, 0x1aa, r);                          /* CMD8 */
    if (h < 0)
        return 3;
    hcs = responded(h) && (r32(r) & 0xfff) == 0x1aa ? 0x40000000 : 0;
    t = millis();
    for (;;) {
        pause(20);
        if (!responded(card_cmd(0x4c37, 0, r)))              /* CMD55 */
            return 4;
        if (!responded(card_cmd(0x4c29, hcs | 0x00ff0000, r))) /* ACMD41 */
            return 5;
        ocr = r32(r);
        if (ocr & 0x80000000)
            break;
        if (millis() - t > 6000)
            return 6;
    }
    high_capacity = (ocr & 0x40000000) != 0;
    if (!responded(card_cmd(0x4c42, 0, r)))                  /* CMD2 */
        return 7;
    if (!responded(card_cmd(0x4c03, 0, r)))                  /* CMD3 */
        return 8;
    rca = (u32)(r[0] & 0xff) << 8 | r[1] >> 8;
    if (!responded(card_cmd(0x4c49, rca << 16, r)))          /* CMD9 */
        return 9;
    if (setctl(CTL_FAST))
        return 10;
    if (!responded(card_cmd(0x4c07, rca << 16, r)))          /* CMD7 */
        return 11;
    if (!responded(card_cmd(0x4c10, 0x200, r)))              /* CMD16 */
        return 12;
    return 0;
}

/* The processor's program loaded, its pins as the stock program had
 * them, and the card started. */
static int take_over(void)
{
    int r = dsp_load();
    if (r)
        return 100 - r;
    if (pins_known) {
        /* The latch first, then the directions (bit 13: change them,
         * bit 12: to outputs, the low byte: which).  Pin 2 high: it is
         * the stock program's interrupt request, low while a message
         * waits. */
        if (wrreg(0x3804, (u16)((pins_data | 4) & 0xff)) || wrreg(0x3805, (u16)(0x3000 | (pins_dir & 0xff))))
            return 110;
    }
    return card_start();
}

/* ---- reading ------------------------------------------------------- */

/* BURST_PACE (EXTRA_CFLAGS=-DBURST_PACE=8; tests/linkbench.rs): so
 * many instructions after each read of a burst.  For the emulator, whose
 * port reads take one cycle of the CPU (4 ns) where a unit's bus takes
 * two of its own at least (34 ns): unpaced, the emulated host outruns
 * the card processor, which a unit cannot be said to do or not to do
 * until it has run this.  The program for a unit is built without. */
#ifndef BURST_PACE
#define BURST_PACE 0
#endif

/* One block: its header, and its 256 words into buf. */
static int block_in(u16 *buf, int burst)
{
    u16 h;
    int i, d;
    if (get(&h, 500))
        return -1;
    if (burst) {
        volatile u16 *p = (volatile u16 *)port;
        /* The first word looked for, the rest not. */
        if (get(&buf[0], 100))
            return -1;
        for (i = 1; i < 256; i++) {
            buf[i] = *p;
            for (d = 0; d < BURST_PACE; d++)
                __asm__ volatile("nop");
        }
    } else {
        for (i = 0; i < 256; i++)
            if (get(&buf[i], 100))
                return -1;
    }
    return h;
}

static void note(struct result *r, int h)
{
    if (h & 0x80)
        r->timeouts++;
    r->status |= (u16)(h & 0x80f8);
}

static u32 address(u32 sector)
{
    return high_capacity ? sector : sector << 9;
}

/* n sectors, a command each. */
static int read_cmd17(u32 sector, u32 n, u8 *to, struct result *r, int burst)
{
    u32 a = address(sector), step = high_capacity ? 1 : 512, i;
    int h;
    if (put(6) || put((u16)(a >> 16)) || put((u16)a) || put((u16)(step >> 16)) || put((u16)step) || put((u16)n))
        return -1;
    for (i = 0; i < n; i++) {
        h = block_in((u16 *)(to + 512 * i), burst);
        if (h < 0)
            return -1;
        note(r, h);
    }
    return done_word();
}

/* n sectors by one command. */
static int read_cmd18(u32 sector, u32 n, u8 *to, struct result *r, int burst)
{
    u32 a = address(sector), i;
    u16 h, late;
    int b;
    if (put(7) || put((u16)(a >> 16)) || put((u16)a) || put((u16)n))
        return -1;
    for (i = 0; i < n; i++) {
        b = block_in((u16 *)(to + 512 * i), burst);
        if (b < 0)
            return -1;
        note(r, b);
    }
    if (get(&h, 1000) || get(&late, 100))
        return -1;
    note(r, h);
    r->late += late;
    return 0;
}

/* sd_read's sectors, for the FAT code, through this program. */
static int one_sector(u32 sector, void *buf)
{
    static u16 bounce[256];
    struct result scratch;
    memset(&scratch, 0, sizeof scratch);
    if (dead || read_cmd17(sector, 1, (u8 *)bounce, &scratch, 0) || scratch.timeouts)
        return -1;
    memcpy(buf, bounce, 512);
    return 0;
}

/* LINKTEST.BIN: 32-bit words, big-endian, word j of the file being
 * pattern(j) (tools/linktest.py, tests/disc.rs). */
static u32 pattern(u32 j)
{
    u32 x = (j + 1) * 0x9e3779b1u;
    x ^= x >> 16;
    x *= 0x85ebca6bu;
    x ^= x >> 13;
    return x;
}

/* The file's first run of contiguous clusters, up to 2 MB, through
 * whatever reads sd_read's sectors now: 1 when found. */
static int find_file(void)
{
    struct sd_file f;
    u32 n = 1, run;
    if (sd_open("LINKTESTBIN", &f, CHAIN, CHAIN_MAX) || f.clusters == 0)
        return 0;
    while (n < f.clusters && CHAIN[n] == CHAIN[n - 1] + 1)
        n++;
    run = n * f.per_cluster;
    if (run > f.size / 512)
        run = f.size / 512;
    if (run > MAX_SECTORS)
        run = MAX_SECTORS;
    if (run == 0)
        return 0;
    first_sector = f.data_start + (CHAIN[0] - 2) * f.per_cluster;
    sectors = run;
    return 1;
}

/* Sectors read that are not what they should be. */
static u32 check(const u8 *buf, u32 n)
{
    u32 s, i, bad = 0;
    for (s = 0; s < n; s++) {
        const u32 *w = (const u32 *)(buf + 512 * s), *ref = (const u32 *)(REF + 512 * s);
        for (i = 0; i < 128; i++)
            if (w[i] != (have_file ? pattern(s * 128 + i) : ref[i]))
                break;
        if (i < 128)
            bad++;
        if ((s & 255) == 255)
            panel_service();
    }
    return bad;
}

/* ---- the tests ----------------------------------------------------- */

static void card_test(int t)
{
    const struct test *T = &tests[t];
    struct result *r = &res[t];
    u32 done = 0, n, t0;
    int rc = 0;
    if (dead || !card_ok || (!have_file && !have_ref && t != 3))
        return;
    r->ran = 1;
    if (setctl(T->ctl)) {
        r->failed = 1;
        return;
    }
    while (done < sectors) {
        n = sectors - done > RUN ? RUN : sectors - done;
        t0 = micros();
        rc = T->multi ? read_cmd18(first_sector + done, n, DATA + 512 * done, r, T->burst)
                      : read_cmd17(first_sector + done, n, DATA + 512 * done, r, T->burst);
        r->us += micros() - t0;
        if (rc)
            break;
        done += n;
        panel_service();
    }
    r->bytes = done * 512;
    if (rc) {
        r->failed = 1;
        return;
    }
    (void)setctl(CTL_FAST);
    if (!have_file && t == 3 && !r->timeouts) {
        /* What CMD17 read is the reference for the rest. */
        memcpy(REF, DATA, sectors * 512);
        have_ref = 1;
    }
    r->bad = check(DATA, done);
    r->checked = 1;
    if (r->timeouts) {
        u16 resp[8];
        /* A block that did not come may have left the card sending. */
        (void)card_cmd(0x4c0c, 0, resp);
    }
}

static void port_out_test(int t)
{
    struct result *r = &res[t];
    u32 op, i, t0, total = MAX_SECTORS / RUN;
    u16 *buf = (u16 *)DATA;
    if (dead)
        return;
    r->ran = 1;
    for (op = 0; op < total; op++) {
        u16 *at = buf + op * RUN * 256;
        t0 = micros();
        if (put(8) || put(RUN)) {
            r->failed = 1;
            break;
        }
        for (i = 0; i < RUN; i++) {
            int h = block_in(at + i * 256, tests[t].burst);
            if (h < 0)
                break;
            if (h != 0x5a5a)
                r->bad++;
        }
        if (i < RUN || done_word()) {
            r->failed = 1;
            break;
        }
        r->us += micros() - t0;
        r->bytes += RUN * 512;
        panel_service();
    }
    /* Each message's words count from 0. */
    for (op = 0; op < r->bytes / (RUN * 512); op++)
        for (i = 0; i < RUN; i++) {
            u32 k;
            const u16 *b = buf + (op * RUN + i) * 256;
            for (k = 0; k < 256; k++)
                if (b[k] != (u16)(i * 256 + k))
                    break;
            if (k < 256)
                r->bad++;
        }
    r->checked = 1;
}

static void port_in_test(int t)
{
    struct result *r = &res[t];
    u32 op, i, t0, total = MAX_SECTORS / RUN;
    u16 sum, got;
    if (dead)
        return;
    r->ran = 1;
    for (op = 0; op < total; op++) {
        sum = 0;
        t0 = micros();
        if (put(9) || put(RUN)) {
            r->failed = 1;
            break;
        }
        for (i = 0; i < RUN * 256; i++) {
            u16 w = (u16)(i * 0x9e37u + op);
            sum = (u16)(sum + w);
            if (put(w))
                break;
        }
        if (i < RUN * 256 || get(&got, 1000) || done_word()) {
            r->failed = 1;
            break;
        }
        r->us += micros() - t0;
        r->bytes += RUN * 512;
        if (got != sum)
            r->bad += RUN;
        panel_service();
    }
    r->checked = 1;
}

/* The stock way, for comparison: the installed software's program, a
 * message a sector, over the same sectors (1 MB of them at most). */
static void stock_test(int t)
{
    struct result *r = &res[t];
    u32 s, n = sectors > 2048 ? 2048 : sectors, t0;
    r->ran = 1;
    for (s = 0; s < n; s++) {
        t0 = micros();
        if (sd_read(first_sector + s, DATA + 512 * s)) {
            r->failed = 1;
            break;
        }
        r->us += micros() - t0;
        if ((s & 31) == 31)
            panel_service();
    }
    r->bytes = s * 512;
    if (have_file) {
        r->bad = check(DATA, s);
        r->checked = 1;
    }
}

static u32 kbs(const struct result *r)
{
    return r->us ? r->bytes * 1000 / r->us : 0;
}

static void report(int t)
{
    const struct result *r = &res[t];
    char text[64], *q;
    int pad;
    u32 k = kbs(r);
    q = put_str(text, tests[t].name);
    for (pad = 0; tests[t].name[pad]; pad++)
        ;
    for (; pad < 15; pad++)
        *q++ = ' ';
    if (!r->ran) {
        q = put_str(q, "NOT RUN");
    } else {
        q = put_dec(q, k);
        q = put_str(q, " KB/S ");
        q = put_dec(q, k * 8 / 1000);
        *q++ = '.';
        q = put_dec(q, k * 8 / 100 % 10);
        q = put_str(q, " B");
        if (r->checked)
            q = put_dec(q, r->bad);
        else
            *q++ = '-';
        q = put_str(q, " T");
        q = put_dec(q, r->timeouts);
        q = put_str(q, " L");
        q = put_dec(q, r->late);
        if (r->status & ~0x80u) {
            q = put_str(q, " S");
            q = put_hex(q, r->status, 4);
        }
        if (r->failed)
            q = put_str(q, " FAIL");
    }
    *q = 0;
    line(text);
    rec((u8)(0x60 + t), 0, r->bytes, (u32)r->ran | (u32)r->failed << 1 | (u32)r->checked << 2);
    rec((u8)(0x60 + t), 1, r->bad, r->timeouts);
    rec((u8)(0x60 + t), 2, r->late, r->status);
    rec((u8)(0x60 + t), 3, k, k * 8 / 100);
    rec((u8)(0x60 + t), 4, r->us, 0);
}

/* RETURN held (panel_service, anywhere): the processor back into reset
 * before the loader takes over; the loader starts it anew. */
static void leave(void)
{
    sd_reader = 0;
    dsp_reset();
}

/* After a test the processor did not get through: everything again. */
static void revive(void)
{
    if (!dead)
        return;
    line("  THE CARD PROCESSOR STOPPED: STARTING IT AGAIN");
    card_ok = take_over() == 0;
}

int main(void)
{
    char text[64], *q;
    int stock = -1, r, t;

    board_init();
    console_puts("\nrnse sdk: linkbench\n");
    if (screen_width != 400) {
        console_puts("the 192 only\n");
        return 1;
    }
    screen_palette(BLACK, 0, 0, 0);
    screen_palette(WHITE, 255, 255, 255);
    screen_fill(0, 0, screen_width, screen_height, BLACK);
    can_init();
    panel_init();
    sd_idle = panel_service;
    display_idle = panel_service;
    loader_leave = leave;
    line("RNSE SDK: LINKBENCH 1.  RESULTS ON CAN 7F8");
    line("HOLD RETURN 2 S: BACK TO THE LOADER");

    for (slot = 0; slot < 2 && !sd_present(slot); slot++)
        ;
    if (slot == 2) {
        slot = 0;
        line("NO CARD IN EITHER SLOT: THE PORT TESTS ONLY");
    }
    port = ports[slot];
    none_bit = slot ? 1 : 4;
    full_bit = slot ? 2 : 8;

    /* The stock program first, if it is there: the pins, the file and
     * the stock way's time. */
    stock = sd_attach(slot);
    if (stock)
        stock = sd_init(slot);
    if (stock == 0) {
        pins_known = sd_peek(0, 0x3804, &pins_data) == 0 && sd_peek(0, 0x3805, &pins_dir) == 0;
        have_file = find_file();
    }
    rec(0x5f, 0, (u32)slot, (u32)stock);
    rec(0x5f, 1, pins_data, (u32)pins_known << 15 | pins_dir);
    q = put_str(text, "SLOT ");
    q = put_dec(q, (u32)slot + 1);
    q = put_str(q, stock == 0 ? ", STOCK PROGRAM: PINS " : ", NO STOCK PROGRAM (");
    if (stock == 0) {
        q = put_hex(q, pins_data, 2);
        *q++ = '/';
        q = put_hex(q, pins_dir, 2);
    } else {
        if (stock < 0) {
            *q++ = '-';
            q = put_dec(q, (u32)-stock);
        } else {
            q = put_dec(q, (u32)stock);
        }
        *q++ = ')';
    }
    *q = 0;
    line(text);
    if (have_file) {
        line("  TIMING THE STOCK WAY");
        stock_test(0);
    }

    line("LOADING THE PROGRAM, STARTING THE CARD");
    r = take_over();
    card_ok = r == 0;
    rec(0x5f, 2, (u32)r, (u32)(ocr >> 16));
    q = put_str(text, r == 0 ? "  CARD STARTED, OCR " : "  FAILED AT STEP ");
    q = r == 0 ? put_hex(q, ocr, 8) : put_dec(q, (u32)r);
    *q = 0;
    line(text);
    if (r >= 90) {
        line("  THE PROCESSOR DID NOT TAKE THE PROGRAM");
    } else {
        if (card_ok && !have_file) {
            sd_reader = one_sector;
            have_file = find_file();
            sd_reader = 0;
        }
        if (card_ok && !have_file) {
            first_sector = 0;
            sectors = MAX_SECTORS;
        }
        rec(0x5f, 3, first_sector, sectors);
        rec(0x5f, 4, (u32)have_file, (u32)high_capacity);
        q = put_str(text, have_file ? "  LINKTEST.BIN: " : "  NO LINKTEST.BIN: THE CARD'S FIRST ");
        q = put_dec(q, sectors);
        q = put_str(q, " SECTORS");
        *q = 0;
        if (card_ok)
            line(text);
        for (t = 1; t < TESTS; t++) {
            if (tests[t].kind == PORT_OUT)
                port_out_test(t);
            else if (tests[t].kind == PORT_IN)
                port_in_test(t);
            else
                card_test(t);
            revive();
        }
    }
    leave();

    screen_fill(0, 0, screen_width, screen_height, BLACK);
    row = 0;
    line("LINKBENCH 1: KB/S, MBIT/S, BAD SECTORS,");
    line("BLOCKS NOT IN TIME, LATE BLOCKS, STATUS");
    for (t = 0; t < TESTS; t++)
        report(t);
    rec(0xff, 0xff, millis(), 0);
    /* The stock program back, as the loader expects to find it. */
    if (stock == 0) {
        if (sd_init(slot))
            line("THE STOCK PROGRAM DID NOT START AGAIN");
        else
            loader_leave = 0;
    }
    line("");
    line("DONE.  BACK TO THE SD LOADER IN 60 S");
    {
        u32 t0 = millis();
        while (millis() - t0 < 60000) {
            panel_service();
            if (panel_return_held())
                line("NO LOADER TO RETURN TO");
            cpu_idle();
        }
    }
    return 0;
}
