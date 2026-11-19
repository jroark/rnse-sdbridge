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
#define DUMPS ((u8 *)0x8cd00000)         /* each test's first bad sector */
#define NEXT ((u8 *)0x8ce00000)          /* LINKBENC.BIN of the other card */
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
 * It says 1b01 when it has started.
 *
 * Every word for the host goes by a plain write to HDT after HST bit 1
 * (a word not yet taken) has been seen clear ("wait", "put" at the
 * end), and every word from it by a plain read.  The first version sent
 * the response's words, the sectors and the port test's words by REP
 * with a parallel move into HDT through a pointer, counting on HST.HAWE
 * to hold each write until the host had read the one before: the
 * emulator holds them, but on a unit (2026-11-17, the RP2040 bridge in
 * the slot) the host got the header of CMD0's answer and not its eight
 * words, and the card never saw a CMD8.  The stock program and
 * src/pcm.c, which work there, only make plain accesses to HDT. */
static const u32 program[] = {
    /* 0x200: the reset vector, three more, and interrupts 0-11 from
     * 0x210, four words each.  Each interrupt sets a word at 0x0f90 + n
     * (to r7l, the card interface's setting, never 0), so that the
     * host sees which came for each command of the card's start; 10,
     * the card's response, also sets the flag at 0x0f80 that the
     * program waits for, as the stock program does.  0-3, 6, 7 and 11
     * mask themselves when they come (in case they come again and again);
     * the host port's (8, 9) and the serial ones stay masked.  (On a
     * unit, 2026-11-18, ACMD41's answer raised no 10 with linkbench,
     * where the stock program gets its answer: which interrupt it is
     * is what these say.) */
    0x2c002000, 0, 0, 0,          /* jmp 0x240 */
    0x26000000, 0, 0, 0, 0x26000000, 0, 0, 0, 0x26000000, 0, 0, 0,
    0x4b900f90, 0x38400bff, 0x26000000, 0,   /* 0x210, 0: INT1 (masks itself) */
    0x4b900f91, 0x38400bff, 0x26000000, 0,   /* 0x214, 1: INT2 (masks itself) */
    0x4b900f92, 0x38400bff, 0x26000000, 0,   /* 0x218, 2: INT3 (masks itself) */
    0x4b900f93, 0x38400bff, 0x26000000, 0,   /* 0x21c, 3: INT4 (masks itself) */
    0x4b900f94, 0x26000000, 0, 0,   /* 0x220, 4: serial in */
    0x4b900f95, 0x26000000, 0, 0,   /* 0x224, 5: serial out */
    0x4b900f96, 0x38400bff, 0x26000000, 0,   /* 0x228, 6: card data in (masks itself) */
    0x4b900f97, 0x38400bff, 0x26000000, 0,   /* 0x22c, 7: card data out (masks itself) */
    0x4b900f98, 0x26000000, 0, 0,   /* 0x230, 8: host in */
    0x4b900f99, 0x26000000, 0, 0,   /* 0x234, 9: host out */
    0x4b900f80, 0x4b900f9a, 0x26000000, 0,   /* 0x238, 10: the card's response: the flag at 0x0f80 too */
    0x4b900f9b, 0x38400bff, 0x26000000, 0,   /* 0x23c, 11: timer (masks itself) */
    0x61000000,     /* r0 = 0 */
    0x61040000,     /* r1 = 0 */
    0x61080000,     /* r2 = 0 */
    0x610c0000,     /* r3 = 0 */
    0x61100000,     /* r4 = 0 */
    0x61140000,     /* r5 = 0 */
    0x61180000,     /* r6 = 0 */
    0x611c0000,     /* r7 = 0 */
    0x38400330,     /* sr = 0x0330: all but 4, 5, 8 and 9 not masked (each marks a word, see above) */
    0x38017fff,     /* r0l = 0x7fff */
    0x3c420000,     /* eir = r0l: interrupts on (as the stock program has it) */
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
    /* 0x0257 main */
    0x61000000,     /* r0 = 0 */
    0x48113806,     /* r0l = hdt: what to do */
    0x59040001,     /* r1 = r0 - 1 */
    0x2c000a91,     /* if r1 = 0: ping */
    0x59040002,     /* r1 = r0 - 2 */
    0x2c000b91,     /* if r1 = 0: wrreg */
    0x59040003,     /* r1 = r0 - 3 */
    0x2c000f11,     /* if r1 = 0: setctl */
    0x59040004,     /* r1 = r0 - 4 */
    0x2c001191,     /* if r1 = 0: rdreg */
    0x59040005,     /* r1 = r0 - 5 */
    0x2c001491,     /* if r1 = 0: cmd */
    0x59040006,     /* r1 = r0 - 6 */
    0x2c004c91,     /* if r1 = 0: read1 */
    0x59040007,     /* r1 = r0 - 7 */
    0x2c005911,     /* if r1 = 0: readm */
    0x59040008,     /* r1 = r0 - 8 */
    0x2c007311,     /* if r1 = 0: portout */
    0x59040009,     /* r1 = r0 - 9 */
    0x2c007e11,     /* if r1 = 0: portin */
    0x3881eeee,     /* r1l = 0xeeee: not known */
    0x2e008600,     /* call wait */
    0x48903806,     /* hdt = r1l */
    0x2c7ff480,     /* jmp main */
    /* 0x026f ping */
    0x3881a5a5,     /* r1l = 0xa5a5 */
    0x2e008400,     /* call wait */
    0x48903806,     /* hdt = r1l */
    0x2c7ff280,     /* jmp main */
    /* 0x0273 wrreg */
    0x61040000,     /* r1 = 0 */
    0x48913806,     /* r1l = hdt: an address of X memory */
    0x3c800000,     /* dp0 = r1l */
    0x48913806,     /* r1l = hdt: the value */
    0x44900000,     /* *dp0 = r1l */
    0x3881f000,     /* r1l = 0xf000: done */
    0x2e007f80,     /* call wait */
    0x48903806,     /* hdt = r1l */
    0x2c7fee00,     /* jmp main */
    /* 0x027c setctl */
    0x611c0000,     /* r7 = 0 */
    0x4b913806,     /* r7l = hdt: the card interface's setting */
    0x4b903814,     /* sdctl = r7l (and its error flags cleared) */
    0x3881f000,     /* r1l = 0xf000: done */
    0x2e007c00,     /* call wait */
    0x48903806,     /* hdt = r1l */
    0x2c7fea80,     /* jmp main */
    /* 0x0283 rdreg */
    0x61040000,     /* r1 = 0 */
    0x48913806,     /* r1l = hdt: an address of X memory */
    0x3c800000,     /* dp0 = r1l */
    0x00000000,     /* nop */
    0x44910000,     /* r1l = *dp0 */
    0x2e007800,     /* call wait */
    0x48903806,     /* hdt = r1l */
    0x2c7fe680,     /* jmp main */
    /* 0x028b cmd */
    0x61040000,     /* r1 = 0 */
    0x48913806,     /* r1l = hdt: the command register's value */
    0x48903811,     /* sdidx = r1l */
    0x48113806,     /* r0l = hdt: the argument, high half */
    0x48103812,     /* agh = r0l */
    0x48113806,     /* r0l = hdt: low half */
    0x48103813,     /* agl = r0l */
    0x2e000800,     /* call issue */
    0x48113814,     /* r0l = sdctl */
    0x8bc00000,     /* r0 = r0 | r6: and 0x80 for a timeout */
    0x2e007180,     /* call wait */
    0x48103806,     /* hdt = r0l: the header */
    0x38003815,     /* dp0 = rpr */
    0x5a200040,     /* r0 = r1 & 0x40: a long response? */
    0x61040000,     /* r1 = 0 */
    0x38810003,     /* r1l = 3: a short one (more reads go round it) */
    0x2c000110,     /* if r0 = 0: short */
    0x38810008,     /* r1l = 8: a long response */
    0x70020800,     /* r0 = *dp0: a word of the response */
    0x2e006f00,     /* call put */
    0x63240000,     /* r1 = r1 - 1 */
    0x2c7ffe99,     /* if r1 != 0: the next */
    0x2c7fdb00,     /* jmp main */
    /* 0x02a2 issue */
    0x61180000,     /* r6 = 0 */
    0x4b110f72,     /* r6l = *0x0f72: the old way (a test flag): no draining either */
    0x5ad8ffff,     /* r6 = r6 & 0xffff */
    0x2c00059e,     /* if r6 != 0: past the draining */
    0x61180000,     /* r6 = 0 */
    0x3b010003,     /* r6l = 3: SDCTL bit 2 (a response in) taken down first, by reading the response register */
    0x48113814,     /* r0l = sdctl */
    0x5a000004,     /* r0 = r0 & 4  3 words at a time while it is up (so that the read position stays where it was), 9 at most: the eighth read since a response */
    0x2c000310,     /* if r0 = 0: down  takes it down (on a unit, 2026-11-18), and the interrupt */
    0x48113815,     /* r0l = rpr  of the next response comes only when it goes up */
    0x48113815,     /* r0l = rpr */
    0x48113815,     /* r0l = rpr */
    0x63d80000,     /* r6 = r6 - 1 */
    0x2c7ffc9e,     /* if r6 != 0: again */
    0x61180000,     /* r6 = 0 */
    0x4b100f80,     /* *0x0f80 = r6l: no response yet */
    0x4b110f72,     /* r6l = *0x0f72: 1: the old way (a test sets it) */
    0x5ad8ffff,     /* r6 = r6 & 0xffff */
    0x2c00021e,     /* if r6 != 0: old */
    0x4b903814,     /* sdctl = r7l: the setting with no command first, which takes SDCTL bit 2 down: the interrupt of a response comes only when it goes up */
    0x5bf84000,     /* r6 = r7 | 0x4000 */
    0x2c000200,     /* jmp go */
    0x4b113814,     /* old: r6l = sdctl: (test flag) the bits 0-2 of SDCTL written back as read, as before */
    0x5ad87f87,     /* r6 = r6 & 0x7f87 */
    0x5bd84000,     /* r6 = r6 | 0x4000 */
    0x4b103814,     /* sdctl = r6l: the command goes out */
    0x5a200400,     /* r0 = r1 & 0x400: a response? */
    0x61180000,     /* r6 = 0 */
    0x3b01ffff,     /* r6l = 0xffff: the time allowed, 37 clocks a round */
    0x2c000710,     /* if r0 = 0: nores */
    /* 0x02c0 resp */
    0x48110f80,     /* r0l = *0x0f80: set by the response's interrupt */
    0x5a000002,     /* r0 = r0 & 2 */
    0x2c000a18,     /* if r0 != 0: came */
    0x48113814,     /* r0l = sdctl: or an error flag (cleared at the start): a response */
    0x5a000058,     /* r0 = r0 & 0x58  that raised no interrupt, as ACMD41's, whose CRC field is all ones (not 0x20, CMD12's stray word) */
    0x2c000898,     /* if r0 != 0: came  (no rep in this loop: 7 ms in all) */
    0x63d80000,     /* r6 = r6 - 1 */
    0x2c7ffc9e,     /* if r6 != 0: resp */
    0x48110f74,     /* r0l = *0x0f74: responses not seen, counted */
    0x62000000,     /* r0 = r0 + 1 */
    0x48100f74,     /* *0x0f74 = r0l */
    0x3b010080,     /* r6l = 0x80: none came */
    0x24000000,     /* ret */
    /* 0x02cd nores */
    0x48113814,     /* r0l = sdctl */
    0x5a004000,     /* r0 = r0 & 0x4000: the command still on the line */
    0x2c000390,     /* if r0 = 0: came */
    0x1c000020,     /* rep 32 */
    0x00000000,     /* nop */
    0x63d80000,     /* r6 = r6 - 1 */
    0x2c7ffd1e,     /* if r6 != 0: nores */
    0x3b010080,     /* r6l = 0x80 */
    0x24000000,     /* ret */
    /* 0x02d6 came */
    0x61180000,     /* r6 = 0 */
    0x24000000,     /* ret */
    /* 0x02d8 arm */
    0x38010000,     /* r0l = 0 */
    0x48103833,     /* pcr = r0l: stopped */
    0x5a800fff,     /* r0 = r4 & 0xfff */
    0x48103830,     /* psar = r0l */
    0x38010100,     /* r0l = 0x100 */
    0x48103831,     /* psr = r0l: 256 words */
    0x38010002,     /* r0l = 2 */
    0x48103833,     /* pcr = r0l: started, into X memory */
    0x24000000,     /* ret */
    /* 0x02e1 filled */
    0x61040000,     /* r1 = 0 */
    0x3881ffff,     /* r1l = 0xffff */
    /* 0x02e3 fl */
    0x48113833,     /* r0l = pcr */
    0x5a000002,     /* r0 = r0 & 2: still running */
    0x2c000490,     /* if r0 = 0: flok */
    0x1c000020,     /* rep 32 */
    0x00000000,     /* nop */
    0x63240000,     /* r1 = r1 - 1 */
    0x2c7ffd19,     /* if r1 != 0: fl */
    0x48110f75,     /* r0l = *0x0f75: blocks not done, counted */
    0x62000000,     /* r0 = r0 + 1 */
    0x48100f75,     /* *0x0f75 = r0l */
    0x5bd80080,     /* r6 = r6 | 0x80: they did not come */
    /* 0x02ee flok */
    0x24000000,     /* ret */
    /* 0x02ef send */
    0x48913814,     /* r1l = sdctl */
    0x9bc40000,     /* r1 = r1 | r6 */
    0x2e004380,     /* call wait */
    0x48903806,     /* hdt = r1l: the header */
    0x61040000,     /* r1 = 0 */
    0x38810100,     /* r1l = 256 */
    0x70021000,     /* r0 = *dp0++ */
    0x4b113807,     /* r6l = hst */
    0x5ad80002,     /* r6 = r6 & 2: the last word not yet taken */
    0x2c7fff1e,     /* if r6 != 0: again */
    0x48203806,     /* hdt = r0h */
    0x63240000,     /* r1 = r1 - 1 */
    0x2c7ffd19,     /* if r1 != 0: the next */
    0x24000000,     /* ret */
    /* 0x02fd read1 */
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
    0x2e003c00,     /* call drain */
    /* 0x0308 r1blk */
    0x2e7fe800,     /* call arm */
    0x38814d11,     /* r1l = 0x4d11: CMD17, a response and a block */
    0x48903811,     /* sdidx = r1l */
    0x49203812,     /* agh = r2h */
    0x49103813,     /* agl = r2l */
    0x2e7fca80,     /* call issue */
    0x2e7fe980,     /* call filled */
    0x38005000,     /* dp0 = 0x5000 */
    0x2e7fef80,     /* call send */
    0xa8680000,     /* r2 = r2 + r3: the next address */
    0x63b40000,     /* r5 = r5 - 1 */
    0x2c7ffa1d,     /* if r5 != 0: r1blk */
    0x3881f000,     /* r1l = 0xf000: done */
    0x2e003180,     /* call wait */
    0x48903806,     /* hdt = r1l */
    0x2c7fa000,     /* jmp main */
    /* 0x0318 readm */
    0x61080000,     /* r2 = 0 */
    0x49213806,     /* r2h = hdt: the address */
    0x49113806,     /* r2l = hdt */
    0x61140000,     /* r5 = 0 */
    0x4a913806,     /* r5l = hdt: how many */
    0x610c0000,     /* r3 = 0: late blocks */
    0x61100000,     /* r4 = 0 */
    0x3a015000,     /* r4l = 0x5000 */
    0x2e002f80,     /* call drain */
    0x2e7fdb80,     /* call arm */
    0x38814d12,     /* r1l = 0x4d12: CMD18 */
    0x48903811,     /* sdidx = r1l */
    0x49203812,     /* agh = r2h */
    0x49103813,     /* agl = r2l */
    0x2e7fbe00,     /* call issue */
    /* 0x0327 rmblk */
    0x2e7fdd00,     /* call filled */
    0x3e000000,     /* dp0 = r4l: this block */
    0x5c900200,     /* r4 = r4 ^ 0x200: the other buffer (not right behind: on a unit the last word of the one at 0x5000 went wrong with the other at 0x5100) */
    0x63b40000,     /* r5 = r5 - 1 */
    0x2c000515,     /* if r5 = 0: rmsend */
    0x48113814,     /* r0l = sdctl */
    0x5a000001,     /* r0 = r0 & 1: words of the next block already there */
    0x626c0018,     /* if r0 != 0: r3 = r3 + 1 */
    0x48110f70,     /* r0l = *0x0f70: rounds to wait before the next block's transfer is set up (the host sets it; 0: none), */
    0x5a00ffff,     /* r0 = r0 & 0xffff  to see whether the last word of this block goes into memory later than */
    0x2c000190,     /* if r0 = 0: no wait  the transfer unit says it is done */
    0x63000000,     /* r0 = r0 - 1 */
    0x2c7fff98,     /* if r0 != 0: again (2 clocks a round) */
    0x2e7fd200,     /* call arm: for the next block */
    /* 0x0335 rmsend */
    0x2e7fdd00,     /* call send */
    0x61180000,     /* r6 = 0 */
    0x2c7ff81d,     /* if r5 != 0: rmblk */
    0x38814c0c,     /* r1l = 0x4c0c: CMD12, a response */
    0x48903811,     /* sdidx = r1l */
    0x38010000,     /* r0l = 0 */
    0x48103812,     /* agh = r0l */
    0x48103813,     /* agl = r0l */
    0x2e7fb280,     /* call issue */
    0x48113814,     /* r0l = sdctl */
    0x8bc00000,     /* r0 = r0 | r6 */
    0x2e001c00,     /* call wait */
    0x48103806,     /* hdt = r0l */
    0x2e001b00,     /* call wait */
    0x49903806,     /* hdt = r3l: late blocks */
    0x48110f73,     /* r0l = *0x0f73: 1: no cleaning after CMD12 (a test sets it) */
    0x5a00ffff,     /* r0 = r0 & 0xffff */
    0x2c7f8898,     /* if r0 != 0: main */
    0x5be00100,     /* r0 = r7 | 0x100: after CMD12 the card interface reset (on a unit, 2026-11-18, now and then a */
    0x48103814,     /* sdctl = r0l  whole command came with every last word of a block stored late otherwise, never */
    0x4b903814,     /* sdctl = r7l  in 192 commands so: the state that does it is set when the command begins) */
    0x61040000,     /* r1 = 0 */
    0x38810001,     /* r1l = 1: and the word the stopped read left taken by a one-word transfer, whatever SDCTL */
    0x2e001e00,     /* call the transfer of drain  bit 0 says (the reset hides it; there was one every time) */
    0x2c7f8500,     /* jmp main */
    /* 0x034e portout */
    0x61140000,     /* r5 = 0 */
    0x4a913806,     /* r5l = hdt: how many */
    0x61040000,     /* r1 = 0: the count, in r1h */
    0x610c0000,     /* r3 = 0 */
    0x39810001,     /* r3l = 1 */
    0x5f6c0010,     /* r3 = r3 << 16: 1 in r3h */
    /* 0x0354 poblk */
    0x38015a5a,     /* r0l = 0x5a5a */
    0x2e001180,     /* call wait */
    0x48103806,     /* hdt = r0l: the header */
    0x61100000,     /* r4 = 0 */
    0x3a010100,     /* r4l = 256 */
    0x4b113807,     /* r6l = hst */
    0x5ad80002,     /* r6 = r6 & 2: the last word not yet taken */
    0x2c7fff1e,     /* if r6 != 0: again */
    0x48a03806,     /* hdt = r1h */
    0x6e640000,     /* r1 += r3 */
    0x63900000,     /* r4 = r4 - 1 */
    0x2c7ffd1c,     /* if r4 != 0: the next */
    0x63b40000,     /* r5 = r5 - 1 */
    0x2c7ff99d,     /* if r5 != 0: poblk */
    0x2e000b00,     /* call wait */
    0x3881f000,     /* r1l = 0xf000: done */
    0x48903806,     /* hdt = r1l */
    0x2c7f7900,     /* jmp main */
    /* 0x0366 portin */
    0x61140000,     /* r5 = 0 */
    0x4a913806,     /* r5l = hdt: how many */
    0x61000000,     /* r0 = 0 */
    0x61080000,     /* r2 = 0 */
    /* 0x036a piblk */
    0x61100000,     /* r4 = 0 */
    0x3a010100,     /* r4l = 256 */
    0x48213806,     /* r0h = hdt */
    0xa8080000,     /* r2 = r2 + r0 */
    0x63900000,     /* r4 = r4 - 1 */
    0x2c7ffe9c,     /* if r4 != 0: the next */
    0x63b40000,     /* r5 = r5 - 1 */
    0x2c7ffc9d,     /* if r5 != 0: piblk */
    0x2e000300,     /* call wait */
    0x49203806,     /* hdt = r2h: the sum */
    0x2e000200,     /* call wait */
    0x3881f000,     /* r1l = 0xf000: done */
    0x48903806,     /* hdt = r1l */
    0x2c7f7000,     /* jmp main */
    /* 0x0378 wait: until the host took the last word (HST bit 1 clear) */
    0x4b113807,     /* r6l = hst */
    0x5ad80002,     /* r6 = r6 & 2: a word still for the host */
    0x2c7fff1e,     /* if r6 != 0: wait */
    0x24000000,     /* ret */
    /* 0x037c put: r0h to the host */
    0x2e7ffe00,     /* call wait */
    0x48203806,     /* hdt = r0h */
    0x24000000,     /* ret */
    /* 0x037f drain: words the card interface keeps for nobody (SDCTL bit 0), taken by one-word buffer transfers into spare memory: reading the data register does not take them on a unit */
    0x48113814,     /* r0l = sdctl */
    0x48100f77,     /* *0x0f77 = r0l: SDCTL as the drain began (for the host) */
    0x61000000,     /* r0 = 0 */
    0x48100f76,     /* *0x0f76 = r0l: words taken, counted below */
    0x61040000,     /* r1 = 0 */
    0x38810004,     /* r1l = 4: at most so many */
    /* 0x0385 dl */
    0x48113814,     /* r0l = sdctl */
    0x5a000001,     /* r0 = r0 & 1: a word there */
    0x2c000b90,     /* if r0 = 0: done */
    0x48110f76,     /* r0l = *0x0f76 */
    0x62000000,     /* r0 = r0 + 1 */
    0x48100f76,     /* *0x0f76 = r0l */
    0x38010000,     /* r0l = 0 */
    0x48103833,     /* pcr = r0l: stopped */
    0x38010f00,     /* r0l = 0xf00 */
    0x48103830,     /* psar = r0l: spare memory (0x5f00) */
    0x38010001,     /* r0l = 1 */
    0x48103831,     /* psr = r0l: one word */
    0x38010002,     /* r0l = 2 */
    0x48103833,     /* pcr = r0l: started */
    0x61180000,     /* r6 = 0 */
    0x3b01ffff,     /* r6l = 0xffff: the time allowed */
    /* 0x0395 dw */
    0x48113833,     /* r0l = pcr */
    0x5a000002,     /* r0 = r0 & 2: still running */
    0x2c000190,     /* if r0 = 0: taken */
    0x63d80000,     /* r6 = r6 - 1 */
    0x2c7ffe1e,     /* if r6 != 0: again */
    /* 0x039a dtaken */
    0x38010000,     /* r0l = 0 */
    0x48103833,     /* pcr = r0l: stopped */
    0x63240000,     /* r1 = r1 - 1 */
    0x2c7ff419,     /* if r1 != 0: the next */
    /* 0x039e dret */
    0x24000000,     /* ret */
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
    /* Rounds (2 of the card processor's clocks each) between a block's
     * transfer ending and the next block's being set up, in CMD18
     * (0x0f70).  Tried on a unit, 2026-11-18, against the last words
     * stored late: 16 to 128 changed nothing (the cause was what CMD12
     * leaves, see "how" 64).  All 0 now. */
    u8 delay;
    /* 4: one command for the whole file, none after the first.  16:
     * commands started the old way (0x0f72): the response register not
     * read before each to take SDCTL bit 2 down, so the response's
     * interrupt, which comes only when it goes up, does not come (on a
     * unit, 2026-11-18: every single-block read 7 ms late).  64: no
     * cleaning after CMD12 (0x0f73): on a unit now and then a whole
     * CMD18 had every block's last word stored late. */
    u8 how;
} tests[] = {
    { "STOCK DRIVER", STOCK, 0, 0, 0, 0, 0 },
    { "PORT OUT", PORT_OUT, 0, 0, 0, 0, 0 },
    { "PORT IN", PORT_IN, 0, 0, 0, 0, 0 },
    { "CMD17 /4", CARD, 0, 0, CTL(3), 0, 0 },
    { "CMD18 /4", CARD, 1, 0, CTL(3), 0, 0 },
    { "CMD18 /3", CARD, 1, 0, CTL(2), 0, 0 },
    { "PORT OUT BURST", PORT_OUT, 0, 1, 0, 0, 0 },
    { "CMD18 /4 BURST", CARD, 1, 1, CTL(3), 0, 0 },
    { "CMD18 /3 BURST", CARD, 1, 1, CTL(2), 0, 0 },
    { "CMD18 /2", CARD, 1, 0, CTL(1), 0, 0 },
    { "CMD18 /2 BURST", CARD, 1, 1, CTL(1), 0, 0 },
    /* At / 5, slow enough for a burst with twice the pause (burst 2)
     * to keep up: the same burst with the pause doubled says whether
     * a block's last word going wrong (on the unit, 2026-11-17) is the
     * host reading too soon. */
    { "CMD18 /5 BURST", CARD, 1, 1, CTL(4), 0, 0 },
    { "CMD18 /5 BUR40", CARD, 1, 2, CTL(4), 0, 0 },
    { "CMD18 /4 B ONE", CARD, 1, 1, CTL(3), 0, 4 },
    /* Comparisons for a unit: the old ways, which on one went wrong
     * (2026-11-18). */
    { "CMD18 /5 B NOC", CARD, 1, 1, CTL(4), 0, 64 },
    { "CMD17 /4 OLD", CARD, 0, 0, CTL(3), 0, 16 },
};
#define TESTS (int)(sizeof tests / sizeof tests[0])

static struct result {
    u32 bytes, us, bad, timeouts, late;
    u16 resp_lost, fill_lost;   /* the program's counts: responses not seen, blocks not done */
    u16 status;
    u8 ran, failed, checked;
    /* The first bad sector, looked at: which, its first wrong word, what
     * came and what was wanted, where in the file the word that came
     * is (its distance in words, or 0x7fffffff when nowhere), the words
     * shifted by how many bits it is (0: not), and of the bad sectors
     * how many were the first of a command's run. */
    u32 first_bad, last_bad, word, got, want, run_starts, run_ends;
    u32 early;             /* a burst's last read with nothing yet to read */
    u32 odd;               /* bad sectors at odd places in a command (the second buffer) */
    s32 found, shift;
} res[TESTS];

static int slot = -1;
static u32 port;
static u8 none_bit, full_bit;
static int dead;                 /* the processor stopped answering */
static u32 load_word;            /* what came instead of its hello or ping (ffffffff: nothing) */
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

/* Everything the screen said, for the results through the bridge. */
static char said[16384];
static u32 said_n;

static void line(const char *text)
{
    int y = 2 + 10 * row, i;
    for (i = 0; text[i] && said_n < sizeof said - 1; i++)
        said[said_n++] = text[i];
    if (said_n < sizeof said - 1)
        said[said_n++] = '\n';
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
    load_word = 0xffffffff;
    if (get(&w, 500))
        return -3;
    load_word = w;
    if (w != 0x1b01)
        return -5;
    load_word = 0xffffffff;
    if (put(1) || get(&w, 100))
        return -4;
    load_word = w;
    if (w != 0xa5a5)
        return -6;
    return 0;
}

static int wrreg(u16 a, u16 v)
{
    return put(2) || put(a) || put(v) || done_word() ? -1 : 0;
}

static int rdreg(u16 a, u16 *v)
{
    return put(4) || put(a) || get(v, 100) ? -1 : 0;
}

static int setctl(u16 v)
{
    return put(3) || put(v) || done_word() ? -1 : 0;
}

/* A card command: the header (SDCTL as the processor saw it after the
 * response, 0x80 set when none came), or -1; the response's words in r. */
/* The response register gives the words of the last response round and
 * round, from where the last read left off: a short response is 3
 * words, a long one 8, and the program reads just so many (on a unit,
 * 2026-11-18, eight reads of each short one put the next a word late,
 * which made a 32 MB card's "busy" look ready). */
static u16 last_h;               /* the last command's header */

static int card_cmd(u16 reg, u32 arg, u16 *r)
{
    u16 h;
    int i, n = reg & 0x40 ? 8 : 3;
    last_h = 0xfffe;
    if (put(5) || put(reg) || put((u16)(arg >> 16)) || put((u16)arg) || get(&h, 1000))
        return -1;
    last_h = h;
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

/* The 32 bits of a short response. */
static u32 r32(const u16 *r)
{
    return (u32)(r[0] & 0xff) << 24 | (u32)r[1] << 8 | r[2] >> 8;
}

/* The card's start, as src/sd.c has the stock driver do it (SD cards
 * of both kinds; no MultiMediaCard): 0, or the step that failed. */
/* What the card said in the last start, for the screen: CMD8's and
 * CMD2's headers (SDCTL, 0x80 when nothing came), the rounds of ACMD41
 * and its last OCR. */
static u16 start_c8, start_c2, start_a41[8], start_c3[3], start_c9;
static u32 start_rounds, start_ocr;
static u32 card_pace = 20;       /* ms between the steps of a start */

/* A command of the start, after the card interface's setting written
 * again, as the stock program does before each round of ACMD41: a
 * setting with no command puts the response register's read position
 * back to its first word (docs/hardware.md), which a long response's 8
 * words may otherwise leave elsewhere. */
static u16 start_ctl;
static u32 rca;                  /* the card's address, from CMD3 */
/* And for each, a line: the command, its header, the interrupts that
 * came (bit n: 0x0f90 + n set) and SDCTL after. */
static int start_cmd(u16 reg, u32 arg, u16 *r)
{
    char text[48], *q;
    u16 v, ctl = 0, came = 0;
    int h, n;
    if (setctl(start_ctl))
        return -1;
    for (n = 0; n < 12; n++)
        if (wrreg((u16)(0x0f90 + n), 0))
            return -1;
    h = card_cmd(reg, arg, r);
    for (n = 0; n < 12; n++)
        if (rdreg((u16)(0x0f90 + n), &v) == 0 && v)
            came |= (u16)(1u << n);
    (void)rdreg(0x3814, &ctl);
    q = put_str(text, "    CMD");
    q = put_dec(q, reg & 0x3fu);
    q = put_str(q, " H");
    q = put_hex(q, (u32)(h < 0 ? 0xfffe : h), 4);
    q = put_str(q, " I");
    q = put_hex(q, came, 3);
    q = put_str(q, " S");
    q = put_hex(q, ctl, 4);
    *q = 0;
    line(text);
    return h;
}

/* Whether a 1 written to SDCTL bit 2 takes it down: SDCTL, the same
 * written back with no command, SDCTL again. */
static void bit2_test(void)
{
    char text[48], *q;
    u16 a = 0, b = 0;
    if (rdreg(0x3814, &a) || wrreg(0x3814, (u16)(a & 0x3f87)) || rdreg(0x3814, &b))
        return;
    q = put_str(text, "    BIT 2: ");
    q = put_hex(q, a, 4);
    q = put_str(q, ", 1 WRITTEN: ");
    q = put_hex(q, b, 4);
    *q = 0;
    line(text);
}

static int card_start(void)
{
    u16 r[8];
    u32 hcs, t;
    int h;
    start_c8 = start_c2 = 0xffff;
    start_c3[0] = start_c3[1] = start_c3[2] = 0;
    start_rounds = start_ocr = 0;
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
    start_c8 = (u16)h;
    hcs = responded(h) && (r32(r) & 0xfff) == 0x1aa ? 0x40000000 : 0;
    t = millis();
    for (;;) {
        pause(card_pace);
        if (!responded(start_cmd(0x4c37, 0, r)))              /* CMD55 */
            return 4;
        if (start_rounds == 0)
            bit2_test();
        if (!responded(start_cmd(0x4c29, hcs | 0x00ff0000, r))) /* ACMD41 */
            return 5;
        start_rounds++;
        start_ocr = ocr = r32(r);
        memcpy(start_a41, r, sizeof start_a41);
        if (ocr & 0x80000000)
            break;
        if (millis() - t > 6000)
            return 6;
    }
    high_capacity = (ocr & 0x40000000) != 0;
    /* The interface's setting again before CMD2, as the stock driver
     * (and src/sd.c) has it, which also clears its error flags.  (A 32
     * MB card that stopped at CMD2 on a unit, 2026-11-17, had not
     * finished its start: ACMD41's "busy" was read a word out of place,
     * see card_cmd.)  A failed start is tried once more at the stock
     * driver's 150 ms a step, and the screen says what each try got. */
    if (setctl(CTL_SLOW))
        return 13;
    pause(card_pace);
    h = start_cmd(0x4c42, 0, r);                              /* CMD2 */
    start_c2 = h < 0 ? 0xfffe : (u16)h;
    if (!responded(h))
        return 7;
    if (!responded(start_cmd(0x4c03, 0, r)))                  /* CMD3 */
        return 8;
    rca = (u32)(r[0] & 0xff) << 8 | r[1] >> 8;
    memcpy(start_c3, r, sizeof start_c3);
    /* CMD9: its answer (the CSD) is not needed here, and on a unit
     * (2026-11-18) the 32 MB card gave none to this program, with the
     * address from CMD3 right; noted, and the start goes on. */
    h = start_cmd(0x4c49, rca << 16, r);                     /* CMD9 */
    start_c9 = h < 0 ? 0xfffe : (u16)h;
    start_ctl = CTL_FAST;
    if (setctl(CTL_FAST))
        return 10;
    if (!responded(start_cmd(0x4c07, rca << 16, r)))          /* CMD7 */
        return 11;
    if (!responded(start_cmd(0x4c10, 0x200, r)))              /* CMD16 */
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
    if (wrreg(0x0f70, 0) || wrreg(0x0f72, 0) || wrreg(0x0f73, 0)) /* card_test sets them */
        return 111;
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

/* BURST_PACE: so many instructions after each read of a burst, which
 * does not look at the status between words.  The card processor puts a
 * word in HDT only once it has seen the last one taken (the program's
 * "send" and "portout" loops: about 8 of its cycles a word, some 120 ns
 * at 67.7 MHz), so each read of a burst waits that long first: 20 turns
 * of the loop below, about twice that on a unit (the emulator, whose
 * port reads take one cycle of the CPU, needs 12).  CMD18 at / 2 wants a
 * word every 480 ns.  What a burst saves is the status read before
 * each word. */
#ifndef BURST_PACE
#define BURST_PACE 20
#endif

/* The first 128 sectors of each test, a character each: '.' right,
 * 'L' only the last word wrong, 'F' only the first, 'X' every word,
 * '?' something else. */
#define MAPPED 128
static char sector_map[TESTS][MAPPED];
/* And each block's header there: '.' nothing, 'T' it did not come in
 * time (0x80), 'O' SDCTL bit 5 (0x20, what a stopped read leaves), 'B'
 * both. */
static char header_map[TESTS][MAPPED];
static u32 first_of_test;
/* For each of a CMD18 test's first 64 commands: 'C' '.' right, 'L'
 * blocks with only the last word wrong and no other, 'X' anything else;
 * 'D' the words the drain took before it; 'S' SDCTL as the drain began,
 * bits 0-2 and 5 (as bit 3) in a hex digit. */
#define COMMANDS 64
static char cmd_map[TESTS][3][COMMANDS];

/* One block: its header, and its 256 words into buf. */
/* Blocks whose last word a burst found not yet there (counted by a
 * look at the status before that read, which a burst does not need). */
static u32 burst_early;

static int block_in(u16 *buf, int burst)
{
    u16 h;
    int i, d, pace = BURST_PACE * burst;
    if (get(&h, 500))
        return -1;
    if (burst) {
        volatile u16 *p = (volatile u16 *)port;
        /* The first word looked for, the rest not. */
        if (get(&buf[0], 100))
            return -1;
        for (i = 1; i < 256; i++) {
            /* The pause before each read: the processor has to see
             * the last word taken before it puts in the next. */
            for (d = 0; d < pace; d++)
                __asm__ volatile("nop");
            if (i == 255 && (REG8(STATUS) & none_bit))
                burst_early++;
            buf[i] = *p;
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
        u32 k = sector - first_of_test + i;
        b = block_in((u16 *)(to + 512 * i), burst);
        if (b < 0)
            return -1;
        note(r, b);
        if (k < MAPPED)
            header_map[r - res][k] = b & 0x80 ? (b & 0x20 ? 'B' : 'T') : (b & 0x20 ? 'O' : '.');
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
static u32 expected(u32 j)
{
    return have_file ? pattern(j) : ((const u32 *)REF)[j];
}

/* What a bad word is: the same word somewhere else in the file, or the
 * stream of words shifted by some bits (+k: k bits early, -k: late). */
static void look_at(struct result *r, u32 j, u32 got)
{
    u32 k, a, b, c, i, n = sectors * 128;
    r->found = 0x7fffffff;
    r->shift = 0;
    for (i = 0; i < n; i++) {
        if (expected(i) == got) {
            r->found = (s32)(i - j);
            break;
        }
        if ((i & 0xffff) == 0xffff)
            panel_service();
    }
    a = j ? expected(j - 1) : 0;
    b = expected(j);
    c = j + 1 < n ? expected(j + 1) : 0;
    for (k = 1; k < 32; k++) {
        if ((b << k | c >> (32 - k)) == got) {
            r->shift = (s32)k;
            break;
        }
        if ((a << (32 - k) | b >> k) == got) {
            r->shift = -(s32)k;
            break;
        }
    }
}


static u32 check(const u8 *buf, u32 n, struct result *r)
{
    u32 s, i, bad = 0;
    r->run_starts = r->run_ends = r->odd = 0;
    for (s = 0; s < n; s++) {
        const u32 *w = (const u32 *)(buf + 512 * s);
        if (s < MAPPED) {
            u32 wrong = 0, at = 0;
            for (i = 0; i < 128; i++)
                if (w[i] != expected(s * 128 + i)) {
                    if (!wrong)
                        at = i;
                    wrong++;
                }
            sector_map[r - res][s] = wrong == 0 ? '.' : wrong == 128 ? 'X' : wrong > 1 ? '?' : at == 127 ? 'L' : at == 0 ? 'F' : '?';
        }
        for (i = 0; i < 128; i++)
            if (w[i] != expected(s * 128 + i))
                break;
        if (i < 128) {
            if (!bad) {
                memcpy(DUMPS + 512 * (u32)(r - res), w, 512);
                r->first_bad = s;
                r->word = i;
                r->got = w[i];
                r->want = expected(s * 128 + i);
                look_at(r, s * 128 + i, w[i]);
            }
            bad++;
            r->last_bad = s;
            if (s % RUN == 0)
                r->run_starts++;
            if (s % RUN == RUN - 1)
                r->run_ends++;
            if (s % RUN & 1)
                r->odd++;
        }
        if ((s & 255) == 255)
            panel_service();
    }
    return bad;
}

/* ---- the stray word ------------------------------------------------ */

/* On a unit a CMD18 stopped by CMD12 leaves a word behind that the next
 * CMD18's first buffer transfer begins with (docs/hardware.md, "SD card
 * processors"), and reading the data register while SDCTL bit 0 is set
 * does not take it.  Four ways of getting rid of it, each tried between
 * two short CMD18s at / 4 in bursts (which were in time on the unit):
 * M0 nothing; M1 one read of the data register; M2 a buffer transfer of
 * one word into spare memory; M3 the interface's reset (SDCTL bit 8),
 * then its setting again.  A line each: SDCTL after the first CMD18 and
 * after the method, the second CMD18's first word, and how many of its
 * two sectors were right.  On the unit (2026-11-17) only M2 was right,
 * and the program's "drain" has done that before every read since: M0
 * and M2 are now right, M1 and M3 still wrong (both clear SDCTL bit 0,
 * and the drain goes by it). */
#define STRAY_METHODS 4
static char stray_text[STRAY_METHODS][48];

static void stray_test(void)
{
    struct result sc;
    u16 c1 = 0, c2 = 0, w;
    u32 m, i, good;
    char *q;
    if (dead || !card_ok || !have_file || sectors < 130)
        return;
    for (m = 0; m < STRAY_METHODS && !dead; m++) {
        memset(&sc, 0, sizeof sc);
        q = put_str(stray_text[m], "M");
        q = put_dec(q, m);
        if (setctl(CTL_FAST) || read_cmd18(first_sector + 64, 2, DATA, &sc, 1) || rdreg(0x3814, &c1)) {
            put_str(q, " FIRST READ FAILED")[0] = 0;
            break;
        }
        if (m == 1) {
            (void)rdreg(0x3810, &w);
        } else if (m == 2) {
            (void)(wrreg(0x3833, 0) || wrreg(0x3830, 0xf00) || wrreg(0x3831, 1) || wrreg(0x3833, 2));
            pause(5);
            (void)wrreg(0x3833, 0);
        } else if (m == 3) {
            (void)wrreg(0x3814, (u16)(CTL_FAST | 0x100));
            pause(5);
            (void)setctl(CTL_FAST);
        }
        if (dead || rdreg(0x3814, &c2) || read_cmd18(first_sector, 2, DATA, &sc, 1)) {
            put_str(q, " SECOND READ FAILED")[0] = 0;
            break;
        }
        for (i = 0, good = 0; i < 2; i++) {
            const u32 *x = (const u32 *)(DATA + 512 * i);
            u32 k;
            for (k = 0; k < 128 && x[k] == pattern(i * 128 + k); k++)
                ;
            good += k == 128;
        }
        q = put_str(q, " C");
        q = put_hex(q, c1, 4);
        *q++ = '/';
        q = put_hex(q, c2, 4);
        *q++ = ' ';
        q = put_hex(q, ((const u32 *)DATA)[0], 8);
        q = put_str(q, good == 2 ? " RIGHT" : " WRONG ");
        if (good != 2)
            q = put_dec(q, 2 - good);
        *q = 0;
        rec(0x6f, (u8)m, ((const u32 *)DATA)[0], (u32)c1 << 16 | c2);
        rec(0x6f, (u8)(0x10 + m), good, 0);
    }
}

/* ---- a bad last word, looked for where it came from --------------- */

/* After a CMD18 whose sectors' last words are not all right: the last
 * word of the last two blocks as the host got them and as they are in
 * the card processor's two buffers still (block 62 of a run of 64 in
 * the first, 0x5000, block 63 in the second, 0x5200), and which
 * command of the test it was.  Up to 16, for the card. */
#define SUSPECTS 16
static struct suspect {
    u8 test, command, bad;
    u16 host62, host63, dsp62, dsp63, want62, want63;
} suspects[SUSPECTS];
static u32 nsuspects;

static void look_at_run(int t, u32 done, u32 n)
{
    struct suspect *x;
    u32 i, bad = 0;
    u16 v;
    for (i = 0; i < n; i++)
        if (((const u16 *)(DATA + 512 * (done + i)))[255] != (u16)expected((done + i) * 128 + 127))
            bad++;
    if (!bad || n != RUN || nsuspects >= SUSPECTS)
        return;
    x = &suspects[nsuspects++];
    x->test = (u8)t;
    x->command = (u8)(done / RUN);
    x->bad = (u8)bad;
    x->host62 = ((const u16 *)(DATA + 512 * (done + 62)))[255];
    x->host63 = ((const u16 *)(DATA + 512 * (done + 63)))[255];
    x->want62 = (u16)expected((done + 62) * 128 + 127);
    x->want63 = (u16)expected((done + 63) * 128 + 127);
    x->dsp62 = rdreg(0x50ff, &v) ? 0xdead : v;
    x->dsp63 = rdreg(0x52ff, &v) ? 0xdead : v;
}

/* ---- the results through the card --------------------------------- */

/* Everything the screen shows and more, as the arguments of CMD16 (the
 * block length: answered, and the card stays where it is), which the
 * RP2040 bridge logs on its USB serial port (sdbridge/src/main.c,
 * "CMD16 xxxxxxxx").  A card of the shop says "block length error" and
 * no more.  Pairs: 7e00ttii then the value of item ii of test tt (0
 * bytes, 1 us, 2 bad sectors, 3 not in time, 4 late, 5 status, 6 first
 * bad, 7 last bad, 8 its first wrong word, 9 what came, 10 what was
 * wanted, 11 bad first of a command, 12 bad last, 13 bad at odd places,
 * 14 early last words); 7e00tt40 then the first bad sector's 128 words;
 * 4c4b0001 first, 4c4b00ff last.  A pause after each: the bridge's log
 * holds 64. */
/* The other slot, when its card has LINKBENC.BIN (the RP2040 bridge,
 * which a new build reaches over USB, sdbridge/link.py): the results go
 * through it, by the stock program there, whatever became of this
 * slot's card, and at the end this program waits for a new
 * LINKBENC.BIN there and runs it.  -1: none. */
static int bridge = -1;
static u32 next_bytes, next_sum;

static u32 fnv(const u8 *p, u32 n)
{
    u32 h = 2166136261u;
    while (n--)
        h = (h ^ *p++) * 16777619u;
    return h;
}

/* LINKBENC.BIN of the bridge's card into NEXT: 0, or the error. */
static int load_next(void)
{
    int r = sd_attach(bridge);
    if (r)
        r = sd_init(bridge);     /* a bridge that has restarted, say */
    if (r == 0)
        r = sd_load("LINKBENCBIN", NEXT, 0x1c000, &next_bytes);
    if (r == 0)
        next_sum = fnv(NEXT, next_bytes);
    return r;
}

static int via_card(u32 v)
{
    u16 r[9];
    int h;
    if (bridge >= 0) {
        h = sd_command(0x4c10, v, r);
        pause(1);
        return h;
    }
    h = card_cmd(0x4c10, v, r);
    pause(2);
    return h < 0 ? -1 : 0;
}

static void through_the_card(void)
{
    int t;
    u32 i;
    if (bridge < 0 && (dead || !card_ok))
        return;
    line(bridge >= 0 ? "  RESULTS THROUGH THE OTHER CARD" : "  RESULTS THROUGH THE CARD");
    if (bridge >= 0 && sd_attach(bridge))
        return;
    if (via_card(0x4c4b0001))
        return;
    for (t = 0; t < TESTS; t++) {
        const struct result *x = &res[t];
        const u32 v[15] = { x->bytes, x->us, x->bad, x->timeouts, x->late, x->status, x->first_bad, x->last_bad,
                            x->word, x->got, x->want, x->run_starts, x->run_ends, x->odd, x->early };
        if (!x->ran)
            continue;
        for (i = 0; i < 15; i++)
            if (via_card(0x7e000000u | (u32)t << 8 | i) || via_card(v[i]))
                return;
        if (x->checked && x->bad) {
            const u32 *w = (const u32 *)(DUMPS + 512 * (u32)t);
            if (via_card(0x7e000040u | (u32)t << 8))
                return;
            for (i = 0; i < 128; i++)
                if (via_card(w[i]))
                    return;
        }
        panel_service();
    }
    /* 7e00ff8k: suspect k: test<<16 | command<<8 | bad, host62<<16 |
     * dsp62, want62, host63<<16 | dsp63, want63. */
    for (i = 0; i < nsuspects; i++) {
        const struct suspect *x = &suspects[i];
        if (via_card(0x7e00ff80u | i) || via_card((u32)x->test << 16 | (u32)x->command << 8 | x->bad)
            || via_card((u32)x->host62 << 16 | x->dsp62) || via_card(x->want62)
            || via_card((u32)x->host63 << 16 | x->dsp63) || via_card(x->want63))
            return;
    }
    /* 7e00fd00, the screen's text four characters a word, 7e00fdff. */
    if (bridge >= 0) {
        if (via_card(0x7e00fd00))
            return;
        for (i = 0; i < said_n; i += 4) {
            u32 w = 0, k;
            for (k = 0; k < 4; k++)
                w = w << 8 | (i + k < said_n ? (u8)said[i + k] : 0);
            if (via_card(w))
                return;
            if ((i & 255) == 0)
                panel_service();
        }
        (void)via_card(0x7e00fdff);
    }
    (void)via_card(0x4c4b00ff);
    (void)via_card(0x200);
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
    burst_early = 0;
    first_of_test = first_sector;
    if (wrreg(0x0f70, T->delay) || wrreg(0x0f72, T->how & 16 ? 1 : 0) || wrreg(0x0f73, T->how & 64 ? 1 : 0) || wrreg(0x0f74, 0) || wrreg(0x0f75, 0) || setctl(T->ctl)) {
        r->failed = 1;
        return;
    }
    while (done < sectors) {
        n = sectors - done > RUN && !(T->how & 4) ? RUN : sectors - done;
        t0 = micros();
        rc = T->multi ? read_cmd18(first_sector + done, n, DATA + 512 * done, r, T->burst)
                      : read_cmd17(first_sector + done, n, DATA + 512 * done, r, T->burst);
        r->us += micros() - t0;
        if (rc)
            break;
        if (T->multi && T->burst && have_file)
            look_at_run(t, done, n);
        if (T->multi && done / RUN < COMMANDS && n == RUN) {
            u32 c = done / RUN, b, k, last = 0, other = 0;
            u16 dn = 0, dc = 0;
            for (b = 0; b < n; b++) {
                const u32 *w = (const u32 *)(DATA + 512 * (done + b));
                u32 wrong = 0, at = 0;
                for (k = 0; k < 128; k++)
                    if (w[k] != expected((done + b) * 128 + k)) {
                        at = k;
                        wrong++;
                    }
                if (wrong == 1 && at == 127)
                    last++;
                else if (wrong)
                    other++;
            }
            (void)rdreg(0x0f76, &dn);
            (void)rdreg(0x0f77, &dc);
            cmd_map[t][0][c] = other ? 'X' : last ? 'L' : '.';
            cmd_map[t][1][c] = (char)('0' + (dn > 9 ? 9 : dn));
            cmd_map[t][2][c] = "0123456789ABCDEF"[(dc & 7) | (dc >> 2 & 8)];
        }
        done += n;
        panel_service();
    }
    (void)rdreg(0x0f74, &r->resp_lost);
    (void)rdreg(0x0f75, &r->fill_lost);
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
    r->bad = check(DATA, done, r);
    r->checked = 1;
    r->early = burst_early;
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
        r->bad = check(DATA, s, r);
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
    int stock = -1, r, t, first_try, shown, i;
    u16 first_c8, first_c2, first_a41[8];
    u32 first_rounds, first_ocr;
    u16 first_last, first_c3[3];

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
    /* A bridge in the other slot, with this program on it. */
    if (slot < 2 && sd_present(slot ^ 1)) {
        bridge = slot ^ 1;
        if (load_next()) {
            bridge = -1;
        } else {
            q = put_str(text, "SLOT ");
            q = put_dec(q, (u32)bridge + 1);
            q = put_str(q, ": LINKBENC.BIN ");
            q = put_hex(q, next_sum, 8);
            q = put_str(q, ", RESULTS THERE");
            *q = 0;
            line(text);
        }
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
    first_try = r;
    first_c8 = start_c8;
    first_c2 = start_c2;
    first_rounds = start_rounds;
    first_ocr = start_ocr;
    first_last = last_h;
    memcpy(first_c3, start_c3, sizeof first_c3);
    memcpy(first_a41, start_a41, sizeof first_a41);
    if (r) {
        /* Once more: a reset of its own first, longer, and the card's
         * start at the stock driver's pace (150 ms a step). */
        dsp_reset();
        pause(200);
        card_pace = 150;
        r = take_over();
    }
    card_ok = r == 0;
    rec(0x5f, 2, (u32)r, (u32)(ocr >> 16));
    rec(0x5f, 5, (u32)first_try, load_word & 0xffff);
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
        line("  WHAT TAKES THE STRAY WORD");
        (void)wrreg(0x0f70, 0);
        (void)wrreg(0x0f72, 0);
        (void)wrreg(0x0f73, 0);
        stray_test();
        if (bridge < 0)
            through_the_card();
    }
    leave();

    screen_fill(0, 0, screen_width, screen_height, BLACK);
    row = 0;
    line("LINKBENCH 1: KB/S, MBIT/S, BAD SECTORS,");
    line("BLOCKS NOT IN TIME, LATE BLOCKS, STATUS");
    for (t = 0; t < TESTS; t++)
        report(t);
    /* Why the program's own tests did not run, when they did not: the
     * lines that said so are gone with the screen above. */
    if (first_try || r) {
        q = put_str(text, "OWN PROGRAM: STEP ");
        q = put_dec(q, (u32)first_try);
        q = put_str(q, ", AGAIN ");
        q = put_dec(q, (u32)r);
        if (load_word != 0xffffffff) {
            q = put_str(q, ", WORD ");
            q = put_hex(q, load_word, 4);
        }
        *q = 0;
        line(text);
        line(r >= 90 ? "  (100+: THE PROGRAM NOT TAKEN; 110: ITS PINS)" : r ? "  (BELOW 90: THE CARD'S START)" : "  (THE SECOND TRY WORKED)");
        /* Each try's start: CMD8's header, ACMD41's rounds and last
         * OCR, CMD2's header (ffff: not got that far). */
        for (t = 0; t < 2; t++) {
            q = put_str(text, t ? "  2ND: C8 " : "  1ST: C8 ");
            q = put_hex(q, t ? start_c8 : first_c8, 4);
            q = put_str(q, " A41 ");
            q = put_dec(q, t ? start_rounds : first_rounds);
            *q++ = ' ';
            q = put_hex(q, t ? start_ocr : first_ocr, 8);
            q = put_str(q, " C2 ");
            q = put_hex(q, t ? start_c2 : first_c2, 4);
            *q = 0;
            line(text);
            /* ACMD41's last answer as the response register gave it
             * (when the start failed: the screen has 23 lines). */
            if (!r)
                continue;
            q = put_str(text, "   ");
            for (i = 0; i < 3; i++) {
                *q++ = ' ';
                q = put_hex(q, t ? start_a41[i] : first_a41[i], 4);
            }
            /* The failed command's header: SDCTL as the program saw it
             * (bit 2: a response in), 0x80 none seen. */
            q = put_str(q, "  LAST ");
            q = put_hex(q, t ? last_h : first_last, 4);
            *q = 0;
            line(text);
            /* CMD3's answer, where the card's address comes from. */
            q = put_str(text, "    C3");
            for (i = 0; i < 3; i++) {
                *q++ = ' ';
                q = put_hex(q, t ? start_c3[i] : first_c3[i], 4);
            }
            *q = 0;
            line(text);
        }
    }
    /* The first bad sector of each card test that had any, looked at:
     * T<test> S<sector> W<word> <came>/<wanted> D<distance in the file
     * of the word that came> K<bits shifted> R<bad first sectors of a
     * run>. */
    /* (The stray word's lines go to the console and CAN only now.) */
    for (t = 0; t < STRAY_METHODS; t++)
        if (stray_text[t][0]) {
            console_puts(stray_text[t]);
            console_puts("\n");
        }
    /* The first bad sector of the card tests that had any, the bursts
     * first (the polled ones lose blocks to lateness, which says
     * enough): T<test> S<first>-<last bad sector> W<word>
     * <came>/<wanted> R<bad first sectors of a command> E<bad last
     * sectors of one> A<bad sectors at even places in a command (the
     * first buffer)>/<at odd places (the second)>, and X<blocks whose
     * last word a burst found not yet there> when there were any. */
    for (r = 0, shown = 0; r < 2; r++)
        for (t = 0; t < TESTS && shown < (first_try ? 2 : 4); t++) {
            const struct result *x = &res[t];
            if (!x->checked || !x->bad || !tests[t].multi || !!tests[t].burst != !r)
                continue;
            q = put_str(text, "T");
            q = put_dec(q, (u32)t);
            q = put_str(q, " S");
            q = put_dec(q, x->first_bad);
            *q++ = '-';
            q = put_dec(q, x->last_bad);
            q = put_str(q, " W");
            q = put_dec(q, x->word);
            *q++ = ' ';
            q = put_hex(q, x->got, 8);
            *q++ = '/';
            q = put_hex(q, x->want, 8);
            q = put_str(q, " R");
            q = put_dec(q, x->run_starts);
            q = put_str(q, " E");
            q = put_dec(q, x->run_ends);
            q = put_str(q, " A");
            q = put_dec(q, x->bad - x->odd);
            *q++ = '/';
            q = put_dec(q, x->odd);
            if (x->early) {
                q = put_str(q, " X");
                q = put_dec(q, x->early);
            }
            *q = 0;
            line(text);
            rec((u8)(0x60 + t), 5, x->first_bad, x->word);
            rec((u8)(0x60 + t), 6, x->got, x->last_bad);
            rec((u8)(0x60 + t), 7, x->want, (u32)x->shift);
            rec((u8)(0x60 + t), 8, (u32)x->found, x->run_starts << 8 | x->run_ends);
            shown++;
        }
    /* The program's own counts for each card test: responses whose
     * interrupt did not come in time, blocks whose transfer did not say
     * it was done. */
    for (t = 0; t < TESTS; t++) {
        if (!res[t].ran || tests[t].kind != CARD)
            continue;
        q = put_str(text, "Q");
        q = put_dec(q, (u32)t);
        q = put_str(q, " RESP ");
        q = put_dec(q, res[t].resp_lost);
        q = put_str(q, " FILL ");
        q = put_dec(q, res[t].fill_lost);
        *q = 0;
        line(text);
    }
    /* The commands' maps (see cmd_map). */
    for (t = 0; t < TESTS; t++) {
        u32 m;
        if (!res[t].ran || !tests[t].multi || !cmd_map[t][0][0])
            continue;
        for (m = 0; m < 3; m++)
            for (i = 0; i < COMMANDS; i += 32) {
                u32 k;
                q = put_str(text, m == 0 ? "C" : m == 1 ? "D" : "S");
                q = put_dec(q, (u32)t);
                *q++ = ' ';
                q = put_dec(q, i);
                *q++ = ' ';
                for (k = 0; k < 32; k++)
                    *q++ = cmd_map[t][m][i + k] ? cmd_map[t][m][i + k] : ' ';
                *q = 0;
                line(text);
            }
    }
    /* The maps (see check), for the multiple-block tests. */
    for (t = 0; t < TESTS; t++) {
        if (!res[t].checked || !tests[t].multi || !res[t].bad)
            continue;
        for (i = 0; i < MAPPED; i += 32) {
            u32 k;
            q = put_str(text, "M");
            q = put_dec(q, (u32)t);
            *q++ = ' ';
            q = put_dec(q, i);
            *q++ = ' ';
            for (k = 0; k < 32; k++)
                *q++ = sector_map[t][i + k] ? sector_map[t][i + k] : ' ';
            *q = 0;
            line(text);
        }
        for (i = 0; i < MAPPED; i += 32) {
            u32 k;
            q = put_str(text, "H");
            q = put_dec(q, (u32)t);
            *q++ = ' ';
            q = put_dec(q, i);
            *q++ = ' ';
            for (k = 0; k < 32; k++)
                *q++ = header_map[t][i + k] ? header_map[t][i + k] : ' ';
            *q = 0;
            line(text);
        }
    }
    rec(0xff, 0xff, millis(), 0);
    /* The stock program back, as the loader expects to find it. */
    if (stock == 0) {
        if (sd_init(slot))
            line("THE STOCK PROGRAM DID NOT START AGAIN");
        else
            loader_leave = 0;
    }
    if (bridge >= 0) {
        u32 mine = next_sum, t0;
        through_the_card();
        q = put_str(text, "DONE.  WAITING FOR A NEW LINKBENC.BIN IN SLOT ");
        q = put_dec(q, (u32)bridge + 1);
        *q = 0;
        line(text);
        line("HOLD RETURN 2 S: BACK TO THE LOADER");
        t0 = millis();
        for (;;) {
            panel_service();
            if (panel_return_held())
                line("NO LOADER TO RETURN TO");
            if (millis() - t0 >= 2000) {
                if (load_next() == 0 && next_sum != mine) {
                    line("A NEW ONE: STARTING IT");
                    (void)run_image(NEXT, next_bytes);
                    line("IT IS NOT A PROGRAM FOR THIS PLACE");
                    mine = next_sum;
                }
                t0 = millis();
            }
            cpu_idle();
        }
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
