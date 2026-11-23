/* SDLINK.UF2: the bridge whole, an RP2040 that is an Android phone's USB
 * host on one side and an SD card in a slot of the RNS-E 192 on the
 * other, the phone's video going from one to the other
 * (docs/sdbridge.md, "The phone's video into the slot").
 *
 * The phone's side is SDSTREAM.UF2's (src/sdhandshake.c): accessory
 * mode, Android Auto's version exchange, the TLS handshake with the
 * certificate the build was given, and the session as an 800x480 head
 * unit (src/aa_hu.c).  The card's side is SDBRIDGE.UF2's (src/main.c):
 * the made-up volume (src/volume.c) with LINKTEST.BIN, and now
 * AALINK.BIN, a window onto the phone's video as a stream of blocks
 * (src/aalink.h, src/aastream.c) that sdk/examples/aaviewer reads.
 *
 * The phone's video messages go into the stream as they are decrypted,
 * frame by frame (a key frame can be bigger than the ring: the 192 reads
 * the first blocks while the last come).  A message is acknowledged to
 * the phone when its last block has gone out to the 192, so the phone
 * sends no faster than the 192 takes pictures.  When the ring has no
 * room for another frame, the phone's bytes wait in USB until the 192
 * reads (the "paused" count).  The phone's sound goes into the stream
 * too, its blocks between the pictures' (dropped a message at a time
 * when the 192 is not reading, or to keep room for the pictures).
 *
 *   core 0  the card's command line and nothing else, from RAM (the
 *           command loop of src/main.c): it has 64 of the host's clocks
 *           to answer, and no interrupt is enabled on this core.
 *   core 1  TinyUSB, TLS and the session from flash in its main loop,
 *           and the card's data side in two interrupts that the main
 *           loop never holds up: a new read from core 0 (the inter-core
 *           FIFO) and a block gone to the data state machine (DMA).
 *           Each makes the next block, from RAM.
 *
 *   GP2  CLK (input)      GP3  CMD      GP4  DAT0      (as SDBRIDGE)
 *   GP0  the log, UART0 TX at 115200 baud
 *   the USB port: the phone, through an OTG adapter, with 5 V on VBUS
 *   (pin 40), never the slot's 3.3 V (only the ground is joined)
 *
 * The LED (src/status.h): WAIT for a phone, BUSY while it switches and
 * the handshake runs, WORKING once it is done, OK while the 192 reads
 * video, FAIL for an error (until the phone is unplugged). */
#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/clocks.h"
#include "hardware/sync.h"
#include "hardware/irq.h"
#include "hardware/vreg.h"
#include "hardware/structs/systick.h"
#include "tusb.h"
#include "sd.pio.h"
#include "card.h"
#include "volume.h"
#include "status.h"
#include "aoa.h"
#include "aa_feed.h"
#include "aa_streamsink.h"
#include "hu_cert.h"

#define CMD_PIN 3
#define DAT0_PIN 4
#ifndef SYS_KHZ
#define SYS_KHZ 250000
#endif
#ifndef AA_MAX_UNACKED
#define AA_MAX_UNACKED 1
#endif
/* The ring of blocks for the 192: what RAM is left (arm-none-eabi-size
 * in sdbridge/build.sh says how much that is). */
#ifndef LINK_BLOCKS
#define LINK_BLOCKS 128
#endif

#if CLK_PIN != 2
#error "CLK_PIN in sd.pio and the wiring here disagree"
#endif

static const PIO pc = pio0, pd = pio1;
static const uint sm_cmd = 0, sm_dat = 0, sm_led = 1;
static uint off_dat;
static int dma_ch;
static spin_lock_t *lock;
static struct card card;

/* ---- the card's command line (core 0) ------------------------------ */

static volatile uint32_t req_gen, req_sector;
static volatile int req_kind;
static volatile uint32_t slowest, reads;

#define LOGRING 64
static volatile uint32_t logring[LOGRING][2];
static volatile uint32_t log_in, log_out, log_lost;

static void __not_in_flash_func(stop_data)(void)
{
    dma_channel_abort((uint)dma_ch);
    pio_sm_set_enabled(pd, sm_dat, false);
    pio_sm_clear_fifos(pd, sm_dat);
    pio_sm_restart(pd, sm_dat);
    pio_sm_exec(pd, sm_dat, pio_encode_set(pio_pindirs, 0));
    pio_sm_exec(pd, sm_dat, pio_encode_jmp(off_dat + sd_dat_offset_start));
    pio_sm_set_enabled(pd, sm_dat, true);
}

static inline uint32_t __not_in_flash_func(cmd_get)(void)
{
    while (pc->fstat & (1u << (PIO_FSTAT_RXEMPTY_LSB + sm_cmd)))
        ;
    return pc->rxf[sm_cmd];
}

static inline void __not_in_flash_func(cmd_put)(uint32_t w)
{
    while (pc->fstat & (1u << (PIO_FSTAT_TXFULL_LSB + sm_cmd)))
        ;
    pc->txf[sm_cmd] = w;
}

static void __attribute__((noinline)) __not_in_flash_func(command_loop)(void)
{
    struct reply r;
    uint8_t cmd[6];
    int i;
    systick_hw->rvr = 0x00ffffff;
    systick_hw->cvr = 0;
    systick_hw->csr = 5;
    for (;;) {
        uint32_t w0 = cmd_get();
        uint32_t w1 = cmd_get();
        uint32_t t0 = systick_hw->cvr, t;
        card_frame(w0, w1, cmd);
        card_command(&card, cmd, &r);
        if (r.nwords)
            for (i = 0; i < r.nwords; i++)
                cmd_put(r.words[i]);
        else
            cmd_put(0);
        t = (t0 - systick_hw->cvr) & 0x00ffffff;
        if (t > slowest)
            slowest = t;
        if (r.read) {
            uint32_t s = spin_lock_blocking(lock);
            stop_data();
            req_kind = r.read > 0 ? r.read : 0;
            req_sector = r.sector;
            req_gen++;
            spin_unlock(lock, s);
            /* Core 1's data side, told (its FIFO interrupt); a full FIFO
             * has told it already. */
            if (sio_hw->fifo_st & SIO_FIFO_ST_RDY_BITS)
                sio_hw->fifo_wr = req_gen;
        }
        if (r.index != 17 && r.index != 18 && r.index != 12 && r.index != 13) {
            uint32_t n = log_in;
            if (n - log_out < LOGRING) {
                logring[n % LOGRING][0] = (uint32_t)r.index | (r.nwords ? 0x100u : 0) | (card.app ? 0x200u : 0);
                logring[n % LOGRING][1] = (uint32_t)cmd[1] << 24 | (uint32_t)cmd[2] << 16 | (uint32_t)cmd[3] << 8 | cmd[4];
                log_in = n + 1;
            } else {
                log_lost++;
            }
        }
    }
}

/* ---- the card's data side (core 1, interrupts) --------------------- */

static uint8_t ring[LINK_BLOCKS][512] __attribute__((aligned(4)));
static struct aastream stream;
static uint32_t link_sector;          /* AALINK.BIN's first sector */

static void __not_in_flash_func(link_sector_words)(uint32_t index, uint32_t *w)
{
    aastream_sector(&stream, index, w);
}

static uint32_t sector_words[128];
static uint32_t blocks[2][CARD_BLOCK_WORDS];
static uint32_t cur_gen = 0xffffffff;
static int cur_kind;
static uint32_t cur_next;             /* the next sector to make */
static int in_dma = -1;               /* the buffer going out, -1 none */
static uint32_t in_dma_sector;
static int ready_buf = -1;            /* the buffer made and waiting, -1 none */
static uint32_t ready_sector;
static volatile uint32_t blocks_sent;
/* When the 192 last started a read of the window (time_us_32). */
static volatile uint32_t last_window_read;

/* A read of the window (not of AALINK.BIN's control or input sectors,
 * whose reads are no place in the stream: with the input counted in, on
 * a unit, 2026-11-18, a turn of the knob let go of pictures not yet read
 * and set acknowledgements free; the 192 then asked for a key frame and
 * the phone's video began anew). */
static int __not_in_flash_func(window_index)(uint32_t sector, uint32_t *index)
{
    return sector >= link_sector && aastream_in_window(sector - link_sector, index);
}

static void __not_in_flash_func(make)(int b, uint32_t sector)
{
    volume_sector(sector, sector_words);
    card_block(sector_words, blocks[b]);
}

/* The block to the data state machine, unless its read has been stopped
 * or replaced meanwhile. */
static int __not_in_flash_func(start)(uint32_t gen, int b)
{
    uint32_t s = spin_lock_blocking(lock);
    int ok = req_gen == gen;
    if (ok)
        dma_channel_transfer_from_buffer_now((uint)dma_ch, blocks[b], CARD_BLOCK_WORDS);
    spin_unlock(lock, s);
    return ok;
}

/* The next block of a multiple read made, while the last goes out. */
static void __not_in_flash_func(make_next)(void)
{
    if (cur_kind != 2 || ready_buf >= 0 || cur_next >= VOLUME_SECTORS)
        return;
    ready_buf = in_dma == 0 ? 1 : 0;
    ready_sector = cur_next++;
    make(ready_buf, ready_sector);
}

static void __not_in_flash_func(send_ready)(void)
{
    if (ready_buf < 0 || in_dma >= 0)
        return;
    if (!start(cur_gen, ready_buf)) {
        ready_buf = -1;
        cur_kind = 0;
        return;
    }
    in_dma = ready_buf;
    in_dma_sector = ready_sector;
    ready_buf = -1;
    make_next();
}

/* A new read (or a stop) from core 0. */
static void __not_in_flash_func(on_request)(void)
{
    uint32_t s, gen, sector, index;
    int kind;
    while (multicore_fifo_rvalid())
        (void)sio_hw->fifo_rd;
    multicore_fifo_clear_irq();
    s = spin_lock_blocking(lock);
    gen = req_gen;
    kind = req_kind;
    sector = req_sector;
    spin_unlock(lock, s);
    if (gen == cur_gen)
        return;
    cur_gen = gen;
    cur_kind = kind;
    in_dma = ready_buf = -1;
    dma_hw->ints0 = 1u << dma_ch;
    if (!kind)
        return;
    reads++;
    /* A read of AALINK.BIN's input or key sector: its event, here where
     * the 192 asked for it (not where a sector is made, which may be
     * ahead of any asking). */
    if (sector >= link_sector)
        aastream_request(&stream, sector - link_sector);
    /* A read of the window: the 192 has what came before. */
    if (window_index(sector, &index)) {
        aastream_consumed(&stream, index);
        last_window_read = time_us_32();
    }
    ready_buf = 0;
    ready_sector = sector;
    cur_next = sector + 1;
    make(0, sector);
    send_ready();
}

/* A block's words all taken by the data state machine. */
static void __not_in_flash_func(on_dma)(void)
{
    uint32_t index;
    dma_hw->ints0 = 1u << dma_ch;
    /* (An aborted transfer can still raise the interrupt, after the next
     * has begun: not done while the channel is busy.) */
    if (in_dma < 0 || dma_channel_is_busy((uint)dma_ch))
        return;
    blocks_sent++;
    if (window_index(in_dma_sector, &index))
        aastream_went_out(&stream, aastream_seq(&stream, index));
    in_dma = -1;
    if (cur_kind == 1) {
        cur_kind = 0;
        return;
    }
    send_ready();
}

/* ---- the phone (core 1, main loop) --------------------------------- */

/* The session and what it puts into the stream: src/aa_feed.c, with the
 * stream in this chip (src/aa_streamsink.c) and the phone's accessory
 * endpoints as its link. */
static struct aa_feed feed;
static struct aa_streamsink streamsink;

enum { WAITING, SWITCHING, RUNNING, FAILED };
static int state = WAITING;
static uint8_t daddr;

static uint64_t usb_bytes;
static uint32_t t_stats;

static uint32_t ms(void)
{
    return to_ms_since_boot(get_absolute_time());
}

static int link_send(void *ctx, const uint8_t *p, uint32_t n)
{
    (void)ctx;
    return aoa_send(daddr, p, n);
}

static uint64_t link_us(void *ctx)
{
    (void)ctx;
    return time_us_64();
}

static int reading(void *ctx)
{
    (void)ctx;
    return time_us_32() - last_window_read < 1000000;
}

static void on_event(void *ctx, int event)
{
    (void)ctx;
    if (event == AA_S_VERSION || event == AA_S_HANDSHAKE)
        status_set(ST_BUSY);
    else if (event == AA_S_AUTHED)
        status_set(ST_WORKING);
    else if (event == AA_S_FAILED)
        status_set(ST_FAIL);
}

/* The USB failed: the session is given up, until the phone is unplugged. */
static void fail(const char *why)
{
    printf("%s\n", why);
    aa_feed_close(&feed);
    state = FAILED;
    status_set(ST_FAIL);
}

/* The phone's bytes: a USB buffer to the feed when it has taken the
 * last, the next transfer under way, into the other buffer, while this
 * one is taken apart. */
static void phone_bytes(void)
{
    if (feed.ses.state == AA_S_FAILED || !aa_feed_idle(&feed) || !aoa_in_done)
        return;
    if (aoa_in_done < 0) {
        fail("reading from the phone failed");
        return;
    }
    usb_bytes += aoa_in_len;
    aa_feed_give(&feed, aoa_in, aoa_in_len);
    if (aoa_read(daddr))
        fail("reading from the phone failed");
}

static const char *const state_names[] = { "idle", "ready", "ident", "stby", "tran", "data" };

/* The card's commands (but the reads) and once a second the counts. */
static void log_card(void)
{
    static uint32_t last_blocks, last_messages, last_reads;
    static uint64_t last_usb, last_decrypt, last_audio;
    uint32_t now = ms(), dt;
    while (log_out != log_in) {
        uint32_t k = log_out % LOGRING;
        uint32_t what = logring[k][0], arg = logring[k][1];
        log_out++;
        printf("%sCMD%lu %08lx%s\n", what & 0x200 ? "A" : "", (unsigned long)(what & 0x3f), (unsigned long)arg,
            what & 0x100 ? "" : "  (no response)");
    }
    dt = now - t_stats;
    if (dt < 1000)
        return;
    {
        uint32_t b = blocks_sent, mhz = clock_get_hz(clk_sys) / 1000000;
        int moved = b != last_blocks;
        printf("card %s, %lu reads, %lu KB/s out (slowest answer %lu of the host's clocks, bad CRC %lu)%s; "
               "phone: %lu pictures/s, USB %lu KB/s, decrypting %lu%%; ring %lu of %u blocks, head %lu, "
               "%lu acks, %lu paused, %lu key frames asked, %lu inputs; sound %lu KB/s, %lu messages dropped; 192 behind %lu%s\n",
            state_names[card.state], (unsigned long)(reads - last_reads),
            (unsigned long)((b - last_blocks) * 512 / 1024 * 1000 / dt),
            (unsigned long)(slowest * 169 / (mhz * 10) + 1), (unsigned long)card.bad_crc,
            log_lost ? " (log lost some)" : "",
            (unsigned long)((stream.messages - last_messages) * 1000 / dt),
            (unsigned long)((usb_bytes - last_usb) * 1000 / 1024 / dt),
            (unsigned long)((feed.ses.decrypt_us - last_decrypt) / 10 / dt),
            (unsigned long)(stream.head - stream.tail), (unsigned)LINK_BLOCKS, (unsigned long)stream.head,
            (unsigned long)feed.ses.hu.v.acks, (unsigned long)feed.pauses, (unsigned long)stream.key_asked,
            (unsigned long)stream.inputs, (unsigned long)((feed.ses.hu.audio_bytes - last_audio) * 1000 / 1024 / dt),
            (unsigned long)feed.sound_dropped, (unsigned long)feed.backlog, feed.dropped_bytes ? " (bytes dropped!)" : "");
        last_blocks = b;
        last_reads = reads;
        last_messages = stream.messages;
        last_usb = usb_bytes;
        last_decrypt = feed.ses.decrypt_us;
        last_audio = feed.ses.hu.audio_bytes;
        t_stats = now;
        if (feed.ses.state == AA_S_AUTHED && feed.streaming)
            status_set(moved ? ST_OK : ST_WORKING);
    }
}

static void phone_gone(void)
{
    aa_feed_close(&feed);
    state = WAITING;
    status_set(ST_WAIT);
    printf("the phone is gone\n");
}

static void core1(void)
{
    /* The data side's interrupts, on this core, before USB's. */
    multicore_fifo_clear_irq();
    irq_set_exclusive_handler(SIO_IRQ_PROC1, on_request);
    irq_set_priority(SIO_IRQ_PROC1, 0x40);
    irq_set_enabled(SIO_IRQ_PROC1, true);
    dma_channel_set_irq0_enabled((uint)dma_ch, true);
    irq_set_exclusive_handler(DMA_IRQ_0, on_dma);
    irq_set_priority(DMA_IRQ_0, 0x40);
    irq_set_enabled(DMA_IRQ_0, true);
    tuh_init(0);
    t_stats = ms();
    for (;;) {
        tuh_task();
        status_tick();
        if (!aoa_device && state != WAITING && state != SWITCHING)
            phone_gone();
        if (aoa_fresh) {
            uint16_t vid = 0, pid = 0;
            aoa_fresh = 0;
            daddr = aoa_device;
            tuh_vid_pid_get(daddr, &vid, &pid);
            printf("device %u: %04x:%04x\n", daddr, vid, pid);
            if (aoa_is_accessory(vid, pid)) {
                usb_bytes = 0;
                state = RUNNING;
                if (aoa_open(daddr) || aa_feed_open(&feed) || aoa_read(daddr))
                    fail("the accessory's endpoints did not work");
            } else {
                status_set(ST_BUSY);
                if (aoa_switch(daddr))
                    fail("no accessory mode");
                else
                    state = SWITCHING;
            }
        }
        if (state == RUNNING)
            phone_bytes();
        aa_feed_run(&feed);
        log_card();
    }
}

static uint32_t core1_stack[16384 / 4];

/* ---- set-up -------------------------------------------------------- */

static void setup_pio(void)
{
    uint off_cmd = pio_add_program(pc, &sd_cmd_program);
    pio_sm_config c;
    gpio_init(CLK_PIN);
    gpio_set_dir(CLK_PIN, GPIO_IN);
    gpio_disable_pulls(CLK_PIN);
    for (uint p = DAT0_PIN + 1; p <= DAT0_PIN + 3; p++) {
        gpio_init(p);
        gpio_set_dir(p, GPIO_IN);
        gpio_disable_pulls(p);
    }
    pio_gpio_init(pc, CMD_PIN);
    gpio_disable_pulls(CMD_PIN);
    pio_sm_set_consecutive_pindirs(pc, sm_cmd, CMD_PIN, 1, false);
    c = sd_cmd_program_get_default_config(off_cmd);
    sm_config_set_in_pins(&c, CMD_PIN);
    sm_config_set_out_pins(&c, CMD_PIN, 1);
    sm_config_set_set_pins(&c, CMD_PIN, 1);
    sm_config_set_jmp_pin(&c, CMD_PIN);
    sm_config_set_in_shift(&c, false, true, 32);
    sm_config_set_out_shift(&c, false, true, 32);
    pio_sm_init(pc, sm_cmd, off_cmd + sd_cmd_offset_start, &c);

    off_dat = pio_add_program(pd, &sd_dat_program);
    pio_gpio_init(pd, DAT0_PIN);
    gpio_disable_pulls(DAT0_PIN);
    pio_sm_set_consecutive_pindirs(pd, sm_dat, DAT0_PIN, 1, false);
    c = sd_dat_program_get_default_config(off_dat);
    sm_config_set_out_pins(&c, DAT0_PIN, 1);
    sm_config_set_set_pins(&c, DAT0_PIN, 1);
    sm_config_set_out_shift(&c, false, true, 32);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
    pio_sm_init(pd, sm_dat, off_dat + sd_dat_offset_start, &c);

    dma_ch = dma_claim_unused_channel(true);
    {
        dma_channel_config d = dma_channel_get_default_config((uint)dma_ch);
        channel_config_set_transfer_data_size(&d, DMA_SIZE_32);
        channel_config_set_read_increment(&d, true);
        channel_config_set_write_increment(&d, false);
        channel_config_set_dreq(&d, pio_get_dreq(pd, sm_dat, true));
        dma_channel_configure((uint)dma_ch, &d, &pd->txf[sm_dat], blocks[0], CARD_BLOCK_WORDS, false);
    }
    pio_sm_set_enabled(pd, sm_dat, true);
    pio_sm_set_enabled(pc, sm_cmd, true);
}

int main(void)
{
    vreg_set_voltage(VREG_VOLTAGE_1_15);
    sleep_ms(2);
    set_sys_clock_khz(SYS_KHZ, true);
    stdio_init_all();
    lock = spin_lock_init((uint)spin_lock_claim_unused(true));
    aastream_init(&stream, ring, LINK_BLOCKS);
    {
        struct aa_session_cert cert = { hu_cert_pem, sizeof hu_cert_pem, HU_CERT_WIRE ? hu_cert_wire : NULL,
                                                     HU_CERT_WIRE ? sizeof hu_cert_wire : 0, hu_key_pem, sizeof hu_key_pem };
        struct aa_sink sink;
        aa_streamsink_init(&streamsink, &stream, reading, NULL);
        aa_streamsink_sink(&streamsink, &sink);
        feed.event = on_event;
        aa_feed_init(&feed, link_send, link_us, NULL, &cert, AA_MAX_UNACKED, &sink);
    }
    link_sector = volume_link_sector();
    volume_link = link_sector_words;
    card_reset(&card);
    setup_pio();
    status_init(pd, sm_led);
    printf("\nsdlink: an SD card of %u MiB with LINKTEST.BIN and AALINK.BIN (the phone's video, %u blocks of RAM), "
           "%lu MHz; waiting for a phone on the USB port (%d frame%s ahead of the acknowledgements)\n",
        VOLUME_SECTORS / 2048, (unsigned)LINK_BLOCKS, (unsigned long)(clock_get_hz(clk_sys) / 1000000), AA_MAX_UNACKED,
        AA_MAX_UNACKED == 1 ? "" : "s");
    multicore_launch_core1_with_stack(core1, core1_stack, sizeof core1_stack);
    /* This core: the command line only, with nothing to interrupt it. */
    command_loop();
}
