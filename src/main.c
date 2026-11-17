/* An RP2040 (a Waveshare RP2040-Zero) as an SD card in a slot of the
 * Audi RNS-E 192: the first step of a bridge to a phone
 * (docs/sdbridge.md, docs/android-auto.md).  This step only reads: the
 * card is the made-up volume of volume.c, LINKTEST.BIN in it, for
 * sdk/examples/linkbench to time and check.
 *
 *   GP2  CLK (input)      GP3  CMD      GP4  DAT0
 *   GP5-7 DAT1-3 (left as inputs: one data line)
 *   the board's LED: GP16 (RP2040-Zero, a WS2812) or GP25 (Pico)
 *
 * Core 1 does nothing but the command line: it takes each command the
 * command state machine (PIO0) pushes, decides with card.c, and pushes
 * the response, fast enough for the 64 clocks a card has.  Core 0 makes
 * the sectors asked for and hands them to the data state machine (PIO1)
 * by DMA, two buffers in turn, and between times keeps the LED and the
 * log on USB (a serial port: commands of the card's start, and counts
 * once a second).  Everything runs from RAM (copy_to_ram), so that no
 * flash read holds up a response.
 *
 * The board takes its power from its own USB connector, never from the
 * slot: only the ground is joined (docs/sdbridge.md). */
#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/clocks.h"
#include "hardware/sync.h"
#include "hardware/vreg.h"
#include "hardware/structs/systick.h"
#include "sd.pio.h"
#include "card.h"
#include "volume.h"
#include "status.h"

#define CMD_PIN 3
#define DAT0_PIN 4
#ifndef SYS_KHZ
#define SYS_KHZ 200000
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

/* What the command loop wants of the data side: a generation number
 * that every new request and every stop changes, and the request. */
static volatile uint32_t req_gen, req_sector;
static volatile int req_kind;

static volatile uint32_t blocks_sent, reads;
/* The longest a response took, in processor clocks from the command's
 * second word to the response's last word in the FIFO (SysTick of core
 * 1): against the 64 clocks of the host's a card has. */
static volatile uint32_t slowest;

/* Commands for the log, from core 1 to core 0. */
#define RING 64
static volatile uint32_t ring[RING][2];
static volatile uint32_t ring_in, ring_out, ring_lost;

/* The data state machine stopped where it is, emptied and started again
 * at its beginning, with DAT0 let go.  With the lock held. */
static void __not_in_flash_func(stop_data)(void)
{
    dma_channel_abort(dma_ch);
    pio_sm_set_enabled(pd, sm_dat, false);
    pio_sm_clear_fifos(pd, sm_dat);
    pio_sm_restart(pd, sm_dat);
    pio_sm_exec(pd, sm_dat, pio_encode_set(pio_pindirs, 0));
    pio_sm_exec(pd, sm_dat, pio_encode_jmp(off_dat + sd_dat_offset_start));
    pio_sm_set_enabled(pd, sm_dat, true);
}

static void __not_in_flash_func(command_loop)(void)
{
    struct reply r;
    uint8_t cmd[6];
    int i;
    systick_hw->rvr = 0x00ffffff;
    systick_hw->cvr = 0;
    systick_hw->csr = 5; /* on, the processor's clock */
    for (;;) {
        uint32_t w0 = pio_sm_get_blocking(pc, sm_cmd);
        uint32_t w1 = pio_sm_get_blocking(pc, sm_cmd);
        uint32_t t0 = systick_hw->cvr, t;
        card_frame(w0, w1, cmd);
        card_command(&card, cmd, &r);
        if (r.nwords)
            for (i = 0; i < r.nwords; i++)
                pio_sm_put_blocking(pc, sm_cmd, r.words[i]);
        else
            pio_sm_put_blocking(pc, sm_cmd, 0);
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
        }
        /* The log: everything but the reads' own commands. */
        if (r.index != 17 && r.index != 18 && r.index != 12 && r.index != 13) {
            uint32_t n = ring_in;
            if (n - ring_out < RING) {
                ring[n % RING][0] = (uint32_t)r.index | (r.nwords ? 0x100u : 0) | (card.app ? 0x200u : 0);
                ring[n % RING][1] = (uint32_t)cmd[1] << 24 | (uint32_t)cmd[2] << 16 | (uint32_t)cmd[3] << 8 | cmd[4];
                ring_in = n + 1;
            } else {
                ring_lost++;
            }
        }
    }
}

/* ---- core 0: the data ---------------------------------------------- */

static uint32_t sector_words[128];
static uint32_t blocks[2][CARD_BLOCK_WORDS];

static void __not_in_flash_func(build)(uint32_t *out, uint32_t n)
{
    volume_sector(n, sector_words);
    card_block(sector_words, out);
}

/* The block to the data state machine, unless the request it belongs to
 * has been stopped or replaced meanwhile. */
static int __not_in_flash_func(send)(uint32_t gen, const uint32_t *b)
{
    uint32_t s = spin_lock_blocking(lock);
    int ok = req_gen == gen;
    if (ok) {
        dma_channel_transfer_from_buffer_now(dma_ch, b, CARD_BLOCK_WORDS);
        blocks_sent++;
    }
    spin_unlock(lock, s);
    return ok;
}

static const char *const state_names[] = { "idle", "ready", "ident", "stby", "tran", "data" };

/* Between requests: the LED and the log. */
static void idle(void)
{
    static uint32_t last_ms, last_blocks, last_crc;
    uint32_t now = to_ms_since_boot(get_absolute_time());
    status_tick();
    while (ring_out != ring_in) {
        uint32_t k = ring_out % RING;
        uint32_t what = ring[k][0], arg = ring[k][1];
        ring_out++;
        printf("%sCMD%lu %08lx%s\n", what & 0x200 ? "A" : "", (unsigned long)(what & 0x3f), (unsigned long)arg,
            what & 0x100 ? "" : "  (no response)");
    }
    if (now - last_ms >= 1000) {
        uint32_t b = blocks_sent;
        int moved = b != last_blocks;
        uint32_t mhz = clock_get_hz(clk_sys) / 1000000;
        printf("state %s  commands %lu  reads %lu  blocks %lu (%lu KB/s)  bad CRC %lu  refused %lu  "
            "slowest answer %lu cycles (%lu of the host's clocks at 16.9 MHz)%s\n",
            state_names[card.state], (unsigned long)card.commands, (unsigned long)reads, (unsigned long)b,
            (unsigned long)((b - last_blocks) / 2), (unsigned long)card.bad_crc, (unsigned long)card.illegal,
            (unsigned long)slowest, (unsigned long)(slowest * 169 / (mhz * 10) + 1), ring_lost ? "  (log lost some)" : "");
        last_blocks = b;
        last_ms = now;
        /* WAIT: not started; OK: started; WORKING: read from in the
         * last second; FAIL: a damaged command in the last second
         * (src/status.h for the colours and patterns). */
        if (card.bad_crc != last_crc)
            status_set(ST_FAIL);
        else if (moved)
            status_set(ST_WORKING);
        else if (card.state >= STBY)
            status_set(ST_OK);
        else
            status_set(ST_WAIT);
        last_crc = card.bad_crc;
    }
}

static void data_loop(void)
{
    for (;;) {
        uint32_t s = spin_lock_blocking(lock), gen = req_gen, sector = req_sector, k;
        int kind = req_kind;
        req_kind = 0;
        spin_unlock(lock, s);
        if (!kind) {
            idle();
            continue;
        }
        reads++;
        build(blocks[0], sector);
        if (!send(gen, blocks[0]) || kind == 1)
            continue;
        /* The next block made while this one goes out. */
        for (k = 1; sector + k < VOLUME_SECTORS; k++) {
            build(blocks[k & 1], sector + k);
            while (dma_channel_is_busy(dma_ch) && req_gen == gen)
                ;
            if (!send(gen, blocks[k & 1]))
                break;
        }
    }
}

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

    /* The command line. */
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

    /* DAT0, with the transmit FIFOs joined: 8 words. */
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

    /* DMA: words to the data state machine at its pace. */
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
    card_reset(&card);
    setup_pio();
    status_init(pd, sm_led);
    multicore_launch_core1(command_loop);
    printf("sdbridge: an SD card of %u MiB with LINKTEST.BIN, %lu MHz\n", VOLUME_SECTORS / 2048,
        (unsigned long)(clock_get_hz(clk_sys) / 1000000));
    data_loop();
}
