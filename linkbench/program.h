/* sdk/examples/linkbench's program for the card processor, shared with
 * sdk/examples/aaviewer, which reads the SD bridge's video through it.
 * Include it in one C file: it defines the table. */
#ifndef LINKBENCH_PROGRAM_H
#define LINKBENCH_PROGRAM_H

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

#endif
