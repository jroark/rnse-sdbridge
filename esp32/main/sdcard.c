/* The ESP32-S3 as the SD card in the unit's slot (sdcard.h).
 *
 * The chip has no programmable I/O, so the two things a card does on
 * the host's clock are done by its peripherals:
 *
 * - The camera half of LCD_CAM reads CMD and DAT0 at each rising edge
 *   of CLK and the DMA controller puts a byte for each into a ring in
 *   memory (bit 0 CMD, bit 1 DAT0; 0xff where nothing is yet).  A loop
 *   on the second core, which does nothing else, finds the commands in
 *   it.  (Bytes reach memory eight at a time, so a command's last bits
 *   wait for up to seven more clocks: the host's clock never stops.)
 * - SPI3 as a slave shifts the response out on the same clock.  Its
 *   select line is tied high or low from the loop: low, it drives CMD
 *   from its buffer; high, it lets the line go.  The buffer begins with
 *   two bits of 1, so that whenever the select falls against the clock
 *   the start bit comes whole.  The loop sees the response go by in the
 *   ring and lets the line go after its end bit.
 * - SPI2 as a slave, fed by DMA, sends the blocks on DAT0 the same way;
 *   the loop finds a block's start bit on the line and lets DAT0 go
 *   after its end.
 *
 * Both change their outputs just after a rising edge of CLK (SPI mode
 * 1), as the RP2040's state machines do (src/sd.pio): the bit stands
 * until the next rising edge, which the host reads at. */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "esp_private/gdma.h"
#include "esp_private/gpio.h"
#include "esp_private/periph_ctrl.h"
#include "esp_rom_gpio.h"
#include "esp_rom_sys.h"
#include "hal/cam_ll.h"
#include "hal/dma_types.h"
#include "hal/gpio_hal.h"
#include "hal/spi_ll.h"
#include "soc/gpio_pins.h"
#include "soc/gpio_sig_map.h"
#include "soc/gpio_struct.h"
#include "soc/lcd_cam_struct.h"
#include "card.h"
#include "crc.h"
#include "volume.h"
#include "sdcard.h"

#define PIN_CMD SDCARD_PIN_CMD
#define PIN_CLK SDCARD_PIN_CLK
#define PIN_DAT0 SDCARD_PIN_DAT0
#define SPARE_CLK 4     /* nothing wired: the camera's start needs a clock */
#define CARD_SPI_MODE 1 /* the outputs change at a rising edge of CLK */

#define NODE 1024
#define NODES 32    /* 32768 clocks: 1.3 ms at 24.9 MHz, what the loop may be kept from its work */
#define RING (NODE * NODES)
#define MASK (RING - 1)

/* Written by the DMA controller behind the compiler's back.  (Not from
 * the heap: Wi-Fi may have left it no piece this long.) */
static uint8_t ring_mem[RING] __attribute__((aligned(4)));
static dma_descriptor_t desc_mem[NODES] __attribute__((aligned(4)));
static volatile uint8_t *ring;
static dma_descriptor_t *desc;
static gdma_channel_handle_t chan;

/* What the loop did, for the log: written by the loop, read by
 * sdcard_report. */
struct rec {
    uint8_t f[6];
    uint32_t cycles;    /* from the command's last bit taken to the select low; 0 no answer */
};
#define RECS 256
static struct rec recs[RECS];
static volatile uint32_t rec_head;
static uint32_t rec_tail;
static volatile uint32_t stat_clocks, stat_commands, stat_slowest, stat_reads, stat_blocks, stat_lost, stat_build;
static struct card card;
static struct sdcard_hooks hooks;

static void capture_init(void)
{
    static const int data_sig[8] = {
        CAM_DATA_IN0_IDX, CAM_DATA_IN1_IDX, CAM_DATA_IN2_IDX, CAM_DATA_IN3_IDX,
        CAM_DATA_IN4_IDX, CAM_DATA_IN5_IDX, CAM_DATA_IN6_IDX, CAM_DATA_IN7_IDX,
    };
    gdma_channel_alloc_config_t alloc = { .direction = GDMA_CHANNEL_DIRECTION_RX };
    gdma_strategy_config_t strategy = { .auto_update_desc = false, .owner_check = false };
    gdma_transfer_config_t transfer = { .max_data_burst_size = 0, .access_ext_mem = false };
    int i;
    ring = ring_mem;
    desc = desc_mem;
    PERIPH_RCC_ACQUIRE_ATOMIC(PERIPH_LCD_CAM_MODULE, ref_count) {
        (void)ref_count;
        cam_ll_enable_bus_clock(0, true);
        cam_ll_reset_register(0);
    }
    ESP_ERROR_CHECK(gdma_new_ahb_channel(&alloc, &chan));
    ESP_ERROR_CHECK(gdma_connect(chan, GDMA_MAKE_TRIGGER(GDMA_TRIG_PERIPH_CAM, 0)));
    ESP_ERROR_CHECK(gdma_apply_strategy(chan, &strategy));
    ESP_ERROR_CHECK(gdma_config_transfer(chan, &transfer));
    memset((void *)ring, 0xff, RING);
    for (i = 0; i < NODES; i++) {
        desc[i].dw0.size = NODE;
        desc[i].dw0.length = 0;
        desc[i].dw0.err_eof = 0;
        desc[i].dw0.suc_eof = 0;
        desc[i].dw0.owner = DMA_DESCRIPTOR_BUFFER_OWNER_DMA;
        desc[i].buffer = (void *)(ring + i * NODE);
        desc[i].next = &desc[(i + 1) % NODES];
    }
    cam_ll_select_clk_src(0, CAM_CLK_SRC_PLL160M);
    cam_ll_set_group_clock_coeff(0, 2, 0, 0);
    cam_ll_enable_stop_signal(&LCD_CAM, 0);
    cam_ll_swap_dma_data_byte_order(&LCD_CAM, 0);
    cam_ll_reverse_dma_data_bit_order(&LCD_CAM, 0);
    cam_ll_enable_vsync_generate_eof(&LCD_CAM, 0);
    cam_ll_enable_hs_line_int(&LCD_CAM, 0);
    cam_ll_enable_vsync_filter(&LCD_CAM, 0);
    cam_ll_set_recv_data_bytelen(&LCD_CAM, NODE - 1);
    cam_ll_enable_invert_pclk(&LCD_CAM, 0);
    cam_ll_set_input_data_width(&LCD_CAM, 8);
    cam_ll_enable_invert_de(&LCD_CAM, 0);
    cam_ll_enable_invert_vsync(&LCD_CAM, 0);
    cam_ll_enable_invert_hsync(&LCD_CAM, 0);
    cam_ll_set_vh_de_mode(&LCD_CAM, 0);
    cam_ll_enable_rgb_yuv_convert(&LCD_CAM, 0);

    gpio_func_sel(SPARE_CLK, PIN_FUNC_GPIO);
    gpio_set_direction(SPARE_CLK, GPIO_MODE_INPUT_OUTPUT);
    gpio_set_level(SPARE_CLK, 0);
    esp_rom_gpio_connect_in_signal(SPARE_CLK, CAM_PCLK_IDX, false);
    esp_rom_gpio_connect_in_signal(PIN_CMD, data_sig[0], false);
    esp_rom_gpio_connect_in_signal(PIN_DAT0, data_sig[1], false);
    for (i = 2; i < 8; i++)
        esp_rom_gpio_connect_in_signal(GPIO_MATRIX_CONST_ZERO_INPUT, data_sig[i], false);
    esp_rom_gpio_connect_in_signal(GPIO_MATRIX_CONST_ZERO_INPUT, CAM_V_SYNC_IDX, false);
    esp_rom_gpio_connect_in_signal(GPIO_MATRIX_CONST_ONE_INPUT, CAM_H_SYNC_IDX, false);
    esp_rom_gpio_connect_in_signal(GPIO_MATRIX_CONST_ONE_INPUT, CAM_H_ENABLE_IDX, false);

    ESP_ERROR_CHECK(gdma_start(chan, (intptr_t)desc));
    cam_ll_reset(&LCD_CAM);
    cam_ll_fifo_reset(&LCD_CAM);
    cam_ll_start(&LCD_CAM);
    /* The camera starts at a pulse of its vertical sync, which it reads
     * by the pixel clock: given on the spare pad, and the slot's clock
     * put in its place after. */
    for (i = 0; i < 12; i++) {
        if (i == 4)
            esp_rom_gpio_connect_in_signal(GPIO_MATRIX_CONST_ONE_INPUT, CAM_V_SYNC_IDX, false);
        if (i == 8)
            esp_rom_gpio_connect_in_signal(GPIO_MATRIX_CONST_ZERO_INPUT, CAM_V_SYNC_IDX, false);
        esp_rom_delay_us(2);
        gpio_set_level(SPARE_CLK, 1);
        esp_rom_delay_us(2);
        gpio_set_level(SPARE_CLK, 0);
    }
    esp_rom_delay_us(2);
    esp_rom_gpio_connect_in_signal(PIN_CLK, CAM_PCLK_IDX, false);
}

#define CMD_SPI SPI_LL_GET_HW(SPI3_HOST)

/* Register values made once, so that each is one write when it counts. */
static uint32_t sel_low, sel_high, fifo_rst, fifo_run, usr_start;

static void response_init(void)
{
    spi_dev_t *hw = CMD_SPI;
    PERIPH_RCC_ATOMIC() {
        spi_ll_enable_bus_clock(SPI3_HOST, true);
        spi_ll_reset_register(SPI3_HOST);
    }
    spi_ll_enable_clock(SPI3_HOST, true);
    esp_rom_gpio_connect_in_signal(GPIO_MATRIX_CONST_ONE_INPUT, SPI3_CS0_IN_IDX, false);
    esp_rom_gpio_connect_in_signal(PIN_CLK, SPI3_CLK_IN_IDX, false);
    esp_rom_gpio_connect_in_signal(GPIO_MATRIX_CONST_ONE_INPUT, SPI3_D_IN_IDX, false);
    spi_ll_slave_init(hw);
    spi_ll_set_rx_lsbfirst(hw, 0);
    spi_ll_set_tx_lsbfirst(hw, 0);
    spi_ll_slave_set_mode(hw, CARD_SPI_MODE, false);
    spi_ll_slave_reset(hw);
    spi_ll_enable_mosi(hw, 1);
    spi_ll_enable_miso(hw, 1);
    spi_ll_apply_config(hw);
    {
        typeof(GPIO.func_in_sel_cfg[0]) g = { .val = 0 };
        typeof(hw->dma_conf) d = hw->dma_conf;
        typeof(hw->cmd) c = { .val = 0 };
        g.sig_in_sel = 1;
        g.func_sel = GPIO_MATRIX_CONST_ZERO_INPUT;
        sel_low = g.val;
        g.func_sel = GPIO_MATRIX_CONST_ONE_INPUT;
        sel_high = g.val;
        d.buf_afifo_rst = 0;
        fifo_run = d.val;
        d.buf_afifo_rst = 1;
        fifo_rst = d.val;
        c.usr = 1;
        usr_start = c.val;
    }
    /* The pad: SPI3's output, driven only while it is selected.  Until
     * the output is connected the pad would drive its own level: 1, as
     * the line is when nobody speaks. */
    gpio_set_level(PIN_CMD, 1);
    gpio_set_direction(PIN_CMD, GPIO_MODE_INPUT_OUTPUT);
    esp_rom_gpio_connect_out_signal(PIN_CMD, SPI3_Q_OUT_IDX, false, false);
}

static inline void IRAM_ATTR cmd_select(int on)
{
    GPIO.func_in_sel_cfg[SPI3_CS0_IN_IDX].val = on ? sel_low : sel_high;
}

/* SPI3 made ready for a response, all but its bits: done while there
 * is time, before a command has come. */
static inline void IRAM_ATTR response_arm(void)
{
    spi_dev_t *hw = CMD_SPI;
    hw->slave.soft_reset = 1;
    hw->slave.soft_reset = 0;
}

static int long_last;   /* the buffer's words 2-5 hold a long response's bits */

/* The response into SPI3's buffer, two bits of 1 before it and 1s
 * after, and the select low. */
static inline void IRAM_ATTR respond(const struct reply *rp)
{
    spi_dev_t *hw = CMD_SPI;
    if (rp->words[0] == 47) {
        uint32_t w0 = rp->words[1], w1 = rp->words[2] | 0xffffu;
        hw->data_buf[0] = __builtin_bswap32(0xc0000000u | w0 >> 2);
        hw->data_buf[1] = __builtin_bswap32(w0 << 30 | w1 >> 2);
        if (long_last) {
            int k;
            for (k = 2; k < 6; k++)
                hw->data_buf[k] = 0xffffffffu;
            long_last = 0;
        }
    } else {
        uint32_t w[6];
        int k;
        for (k = 0; k < 5; k++)
            w[k] = rp->words[1 + k];
        w[4] |= 0x00ffffffu;
        w[5] = 0xffffffffu;
        hw->data_buf[0] = __builtin_bswap32(0xc0000000u | w[0] >> 2);
        for (k = 1; k < 6; k++)
            hw->data_buf[k] = __builtin_bswap32(w[k - 1] << 30 | w[k] >> 2);
        long_last = 1;
    }
    hw->dma_conf.val = fifo_rst;
    hw->dma_conf.val = fifo_run;
    hw->cmd.val = usr_start;
    cmd_select(1);
}

static inline void IRAM_ATTR record(const uint8_t *f, uint32_t cycles)
{
    struct rec *r = &recs[rec_head % RECS];
    memcpy(r->f, f, 6);
    r->cycles = cycles;
    rec_head = rec_head + 1;
}

/* DAT0: SPI2 as a slave on the same clock, its bytes from the DMA
 * controller.  A block is 0xfe (seven clocks of 1 and the start bit),
 * the sector, its CRC16, and 0xff (the end bit, and 1s until the loop
 * has let the line go: with five of them, 39 clocks, a 193 took the
 * fourth block of a CMD18 for damaged; with 17 it took every one). */
#define DAT_SPI SPI_LL_GET_HW(SPI2_HOST)
#define BLOCK_BYTES 532
#define BLOCK_CLOCKS (8 * 516)
static uint8_t *block[2];
static dma_descriptor_t *block_desc;
static gdma_channel_handle_t dat_chan;
static uint32_t dat_low, dat_high, dat_fifo_rst, dat_fifo_run;
static uint32_t sector_words[128];

static void data_init(void)
{
    spi_dev_t *hw = DAT_SPI;
    gdma_channel_alloc_config_t alloc = { .direction = GDMA_CHANNEL_DIRECTION_TX };
    gdma_strategy_config_t strategy = { .auto_update_desc = false, .owner_check = false };
    static uint8_t block_mem[2][BLOCK_BYTES + 4] __attribute__((aligned(4)));
    static dma_descriptor_t block_desc_mem __attribute__((aligned(4)));
    block[0] = block_mem[0];
    block[1] = block_mem[1];
    block_desc = &block_desc_mem;
    PERIPH_RCC_ATOMIC() {
        spi_ll_enable_bus_clock(SPI2_HOST, true);
        spi_ll_reset_register(SPI2_HOST);
    }
    spi_ll_enable_clock(SPI2_HOST, true);
    esp_rom_gpio_connect_in_signal(GPIO_MATRIX_CONST_ONE_INPUT, FSPICS0_IN_IDX, false);
    esp_rom_gpio_connect_in_signal(PIN_CLK, FSPICLK_IN_IDX, false);
    esp_rom_gpio_connect_in_signal(GPIO_MATRIX_CONST_ONE_INPUT, FSPID_IN_IDX, false);
    spi_ll_slave_init(hw);
    spi_ll_set_rx_lsbfirst(hw, 0);
    spi_ll_set_tx_lsbfirst(hw, 0);
    spi_ll_slave_set_mode(hw, CARD_SPI_MODE, true);
    spi_ll_slave_reset(hw);
    spi_ll_enable_mosi(hw, 1);
    spi_ll_enable_miso(hw, 1);
    spi_ll_apply_config(hw);
    ESP_ERROR_CHECK(gdma_new_ahb_channel(&alloc, &dat_chan));
    ESP_ERROR_CHECK(gdma_connect(dat_chan, GDMA_MAKE_TRIGGER(GDMA_TRIG_PERIPH_SPI, 2)));
    ESP_ERROR_CHECK(gdma_apply_strategy(dat_chan, &strategy));
    {
        typeof(GPIO.func_in_sel_cfg[0]) g = { .val = 0 };
        typeof(hw->dma_conf) d;
        hw->dma_conf.dma_tx_ena = 1;
        d = hw->dma_conf;
        g.sig_in_sel = 1;
        g.func_sel = GPIO_MATRIX_CONST_ZERO_INPUT;
        dat_low = g.val;
        g.func_sel = GPIO_MATRIX_CONST_ONE_INPUT;
        dat_high = g.val;
        d.dma_afifo_rst = 0;
        dat_fifo_run = d.val;
        d.dma_afifo_rst = 1;
        dat_fifo_rst = d.val;
    }
    gpio_set_level(PIN_DAT0, 1);
    gpio_set_direction(PIN_DAT0, GPIO_MODE_INPUT_OUTPUT);
    esp_rom_gpio_connect_out_signal(PIN_DAT0, FSPIQ_OUT_IDX, false, false);
}

/* Sector n as a block, in the buffer. */
static void IRAM_ATTR build(uint8_t *b, uint32_t n)
{
    uint32_t *w = (uint32_t *)(b + 4);
    uint16_t crc;
    int i;
    volume_sector(n, sector_words);
    crc = crc16_words(sector_words, 128);
    /* Three bytes of 1 that are not sent, so that the sector's words are
     * on a word's boundary. */
    b[3] = 0xfe;
    for (i = 0; i < 128; i++)
        w[i] = __builtin_bswap32(sector_words[i]);
    b[516] = (uint8_t)(crc >> 8);
    b[517] = (uint8_t)crc;
    memset(b + 518, 0xff, BLOCK_BYTES + 3 - 518);
}

static inline void IRAM_ATTR data_stop(void)
{
    GPIO.func_in_sel_cfg[FSPICS0_IN_IDX].val = dat_high;
}

static void IRAM_ATTR data_start(uint8_t *b)
{
    spi_dev_t *hw = DAT_SPI;
    block_desc->dw0.size = BLOCK_BYTES;
    block_desc->dw0.length = BLOCK_BYTES;
    block_desc->dw0.err_eof = 0;
    block_desc->dw0.suc_eof = 1;
    block_desc->dw0.owner = DMA_DESCRIPTOR_BUFFER_OWNER_DMA;
    block_desc->buffer = b + 3;
    block_desc->next = NULL;
    gdma_reset(dat_chan);
    hw->slave.soft_reset = 1;
    hw->slave.soft_reset = 0;
    hw->dma_conf.val = dat_fifo_rst;
    hw->dma_conf.val = dat_fifo_run;
    hw->dma_int_clr.val = 0xffffffffu;
    gdma_start(dat_chan, (intptr_t)block_desc);
    hw->cmd.usr = 1;
    GPIO.func_in_sel_cfg[FSPICS0_IN_IDX].val = dat_low;
}

#define WORD(at) (*(volatile uint32_t *)(ring + (at)))

/* The 48 bits from the start bit at `start`, into f's 6 bytes: 13
 * words of the ring, 4 bits from each, and the bits before the start
 * bit shifted out.  (The last word may be beyond the command and not
 * yet written: its bits are not used.) */
static inline void IRAM_ATTR gather(uint32_t start, uint8_t *f)
{
    uint32_t at = start & ~3u, hi = 0, lo = 0, sh = start & 3;
    int k;
    for (k = 0; k < 13; k++) {
        uint32_t nib = ((WORD(at) & 0x01010101u) * 0x08040201u) >> 24 & 15;
        hi = hi << 4 | lo >> 28;
        lo = lo << 4 | nib;
        at = (at + 4) & MASK;
    }
    /* 52 bits, the command's from bit 51 - sh down. */
    lo = lo >> (4 - sh) | hi << (28 + sh);
    hi = hi >> (4 - sh);
    f[0] = (uint8_t)(hi >> 8);
    f[1] = (uint8_t)hi;
    f[2] = (uint8_t)(lo >> 24);
    f[3] = (uint8_t)(lo >> 16);
    f[4] = (uint8_t)(lo >> 8);
    f[5] = (uint8_t)lo;
}

/* n bytes of the ring from `at`, taken: 0xff again. */
static inline void IRAM_ATTR taken(uint32_t at, uint32_t n)
{
    while (n) {
        if (!(at & 3) && n >= 4) {
            WORD(at) = 0xffffffffu;
            at = (at + 4) & MASK;
            n -= 4;
        } else {
            ring[at] = 0xff;
            at = (at + 1) & MASK;
            n--;
        }
    }
}

enum { LISTEN, COMMAND, AWAIT, RESPONSE };

static void IRAM_ATTR card_loop(void *arg)
{
    uint32_t r = 0, start = 0, t_end = 0, t_sel = 0, clocks = 0;
    uint32_t sector = 0, d_from = 0, t_data = 0;
    int state = LISTEN, left = 0;
    int reading = 0;        /* 1 a block, 2 blocks until stopped */
    int d_state = 0;        /* 1 a block to go when the response has gone, 2 one going */
    int cur = 0, next_built = 0;
    /* While a block's start bit is looked for, a word of the ring is
     * passed over only if DAT0 is high in it too. */
    uint32_t pass_mask = 0x81818181u, pass_value = 0x01010101u;
    int hunting = 0;
    uint8_t f[6];
    struct reply rp;
    (void)arg;
    memset(f, 0, sizeof f);
    response_arm();
    for (;;) {
        if (state == LISTEN || state == AWAIT) {
            uint8_t b;
            if (!(r & 3)) {
                /* The line idle: words passed over as fast as can be
                 * (3.5 cycles a clock; at 24.9 MHz there are 9.6). */
                volatile uint32_t *base = (volatile uint32_t *)ring;
                uint32_t i = r >> 2, i0 = i, m = pass_mask, v = pass_value;
                while ((base[i] & m) == v) {
                    base[i] = 0xffffffffu;
                    i = (i + 1) & (MASK >> 2);
                }
                clocks += ((i - i0) & (MASK >> 2)) << 2;
                r = i << 2;
            }
            b = ring[r];
            if (b == 0xff) {
                /* All that has come is taken: what waits on the clock's
                 * count is looked at. */
                stat_clocks = clocks;
                if (hunting && esp_cpu_get_cycle_count() - t_data > 4800000u) {
                    /* No start bit on the line in 20 ms. */
                    data_stop();
                    stat_lost = stat_lost + 1;
                    hunting = 0;
                    d_state = 0;
                    reading = 0;
                    pass_mask = 0x81818181u;
                    pass_value = 0x01010101u;
                }
                if (d_state == 2 && !hunting) {
                    /* Half of a block out: the next made, while the
                     * host is taking this one (it sends no command
                     * then; one that came would wait for the making). */
                    if (reading == 2 && !next_built && clocks - d_from > BLOCK_CLOCKS / 2) {
                        uint32_t t = esp_cpu_get_cycle_count();
                        build(block[cur ^ 1], sector + 1);
                        stat_build = esp_cpu_get_cycle_count() - t;
                        next_built = 1;
                    }
                    if (clocks - d_from > 4113 + 12) {
                        data_stop();
                        stat_blocks = stat_blocks + 1;
                        if (hooks.sent)
                            hooks.sent(sector);
                        if (reading == 2 && next_built && sector + 1 < VOLUME_SECTORS) {
                            sector++;
                            cur ^= 1;
                            next_built = 0;
                            data_start(block[cur]);
                            hunting = 1;
                            t_data = esp_cpu_get_cycle_count();
                            pass_mask = 0x83838383u;
                            pass_value = 0x03030303u;
                        } else {
                            d_state = 0;
                            reading = 0;
                        }
                    }
                }
                /* Selected and no clock for 20 ms: the line is let go. */
                if (state == AWAIT && esp_cpu_get_cycle_count() - t_sel > 4800000u) {
                    cmd_select(0);
                    response_arm();
                    state = LISTEN;
                }
                continue;
            }
            if (hunting && !(b & 2)) {
                /* The block's start bit: it ends 4113 clocks on. */
                d_from = clocks;
                hunting = 0;
                pass_mask = 0x81818181u;
                pass_value = 0x01010101u;
            }
            if (b & 1) {
                ring[r] = 0xff;
                r = (r + 1) & MASK;
                clocks++;
                continue;
            }
            /* A start bit: the host's command, or our response as the
             * line has it. */
            start = r;
            state = state == LISTEN ? COMMAND : RESPONSE;
            continue;
        }
        if (state == COMMAND) {
            if (ring[(start + 47) & MASK] == 0xff)
                continue;
            t_end = esp_cpu_get_cycle_count();
            gather(start, f);
            card_command(&card, f, &rp);
            if (rp.read) {
                /* A read, or a stop: whatever was going out ends here. */
                data_stop();
                d_state = 0;
                reading = 0;
                hunting = 0;
                pass_mask = 0x81818181u;
                pass_value = 0x01010101u;
                if (rp.read > 0) {
                    reading = rp.read;
                    sector = rp.sector;
                    d_state = 1;
                }
            }
            if (rp.nwords) {
                respond(&rp);
                t_sel = esp_cpu_get_cycle_count();
                if (t_sel - t_end > stat_slowest)
                    stat_slowest = t_sel - t_end;
                left = (int)rp.words[0] + 1;
                state = AWAIT;
            } else {
                state = LISTEN;
            }
            /* The log: the start's commands and the like, not the reads. */
            if (rp.index == 17 || rp.index == 18 || rp.index == 12 || rp.index == 13)
                stat_reads = stat_reads + 1;
            else
                record(f, rp.nwords ? t_sel - t_end : 0);
            if (rp.read > 0 && hooks.read)
                hooks.read(sector);
            taken(start, 48);
            r = (start + 48) & MASK;
            clocks += 48;
            stat_commands = stat_commands + 1;
            continue;
        }
        /* RESPONSE: its end bit on the line, and the line is let go. */
        if (ring[(start + (uint32_t)left - 1) & MASK] == 0xff) {
            if (esp_cpu_get_cycle_count() - t_sel > 4800000u) {
                cmd_select(0);
                response_arm();
                state = LISTEN;
            }
            continue;
        }
        cmd_select(0);
        response_arm();
        taken(start, (uint32_t)left);
        r = (start + (uint32_t)left) & MASK;
        clocks += (uint32_t)left;
        state = LISTEN;
        if (d_state == 1) {
            /* The block asked for: made now, when the host waits for it
             * and sends nothing. */
            uint32_t t = esp_cpu_get_cycle_count();
            build(block[cur], sector);
            stat_build = esp_cpu_get_cycle_count() - t;
            next_built = 0;
            data_start(block[cur]);
            hunting = 1;
            t_data = esp_cpu_get_cycle_count();
            pass_mask = 0x83838383u;
            pass_value = 0x03030303u;
            d_state = 2;
        }
    }
}

void sdcard_start(const struct sdcard_hooks *h)
{
    if (h)
        hooks = *h;
    gpio_func_sel(PIN_CMD, PIN_FUNC_GPIO);
    gpio_func_sel(PIN_CLK, PIN_FUNC_GPIO);
    gpio_func_sel(PIN_DAT0, PIN_FUNC_GPIO);
    gpio_set_direction(PIN_CMD, GPIO_MODE_INPUT);
    gpio_set_direction(PIN_CLK, GPIO_MODE_INPUT);
    gpio_set_direction(PIN_DAT0, GPIO_MODE_INPUT);
    gpio_set_pull_mode(PIN_CMD, GPIO_FLOATING);
    gpio_set_pull_mode(PIN_CLK, GPIO_FLOATING);
    gpio_set_pull_mode(PIN_DAT0, GPIO_FLOATING);
    card_reset(&card);
    capture_init();
    response_init();
    data_init();
    /* The second core's, and nothing else's: as high as its ipc task,
     * which the system needs there now and then (a write to the flash). */
    xTaskCreatePinnedToCore(card_loop, "card", 4096, NULL, configMAX_PRIORITIES - 1, NULL, 1);
}

void sdcard_report(int commands)
{
    static const char *const states[] = { "idle", "ready", "ident", "stby", "tran", "data" };
    static uint32_t clocks_before, blocks_before;
    uint32_t clocks = stat_clocks, blocks = stat_blocks;
    int lines = 0;
    if (rec_head - rec_tail > RECS)
        rec_tail = rec_head - RECS;
    while (rec_tail != rec_head) {
        const struct rec *q = &recs[rec_tail++ % RECS];
        int ok = crc7(q->f, 5) == q->f[5] >> 1;
        if (!commands || lines++ >= 60)
            continue;
        printf("CMD%d %02x%02x%02x%02x%s", q->f[0] & 0x3f, q->f[1], q->f[2], q->f[3], q->f[4], ok ? "" : " bad CRC");
        if (q->cycles)
            printf("  answered after %lu cycles\n", (unsigned long)q->cycles);
        else
            printf("  no answer\n");
    }
    printf("card: %s  clock %lu kHz  commands %lu  reads %lu  blocks %lu (%lu KB/s)  bad CRC %lu  refused %lu  lost %lu  "
           "slowest answer %lu cycles  a block made in %lu\n",
        states[card.state], (unsigned long)((clocks - clocks_before) / 1000), (unsigned long)stat_commands,
        (unsigned long)stat_reads, (unsigned long)blocks, (unsigned long)((blocks - blocks_before) / 2),
        (unsigned long)card.bad_crc, (unsigned long)card.illegal, (unsigned long)stat_lost, (unsigned long)stat_slowest,
        (unsigned long)stat_build);
    clocks_before = clocks;
    blocks_before = blocks;
}
