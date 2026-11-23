/* The slot's three wires, listened to: nothing here drives GPIO 11, 12
 * or 13.
 *
 * The camera half of LCD_CAM takes a byte at each rising edge of its
 * pixel clock and hands it to the DMA controller; with the slot's CLK
 * as that clock and CMD and DAT0 as two of its data lines, memory fills
 * with a byte for each of the host's clocks (bit 0 CMD, bit 1 DAT0; the
 * other six lines are tied to 0).  The buffer is a ring, filled with
 * 0xff where nothing has been written, so what has come is known by
 * looking.
 *
 * First a test on two spare pads (GPIO 4 the clock, GPIO 5 the data,
 * both driven from here, nothing wired to them): how many clocks pass
 * before a byte is in memory.  Then the slot: the clock's rate, the
 * lines' levels and the commands the unit sends, a line a second. */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/pulse_cnt.h"
#include "esp_heap_caps.h"
#include "esp_private/gdma.h"
#include "esp_private/periph_ctrl.h"
#include "esp_private/gpio.h"
#include "esp_rom_gpio.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "hal/cam_ll.h"
#include "hal/spi_ll.h"
#include "hal/dma_types.h"
#include "hal/gpio_hal.h"
#include "soc/gpio_sig_map.h"
#include "soc/gpio_pins.h"
#include "soc/lcd_cam_struct.h"
#include "soc/gdma_struct.h"
#include "crc.h"

#define PIN_CMD 11
#define PIN_CLK 12
#define PIN_DAT0 13
#define SELF_CLK 4
#define SELF_D 5

#define NODE 1024
#define NODES 64
#define RING (NODE * NODES)

static uint8_t *ring;
static dma_descriptor_t *desc;
static gdma_channel_handle_t chan;
static uint32_t rd;

static void input_pad(int pin)
{
    gpio_func_sel(pin, PIN_FUNC_GPIO);
    gpio_set_direction(pin, GPIO_MODE_INPUT);
    gpio_set_pull_mode(pin, GPIO_FLOATING);
}

static int sync_mode = 0;
static void capture_start(int clk, int d0, int d1, int burst, int vh_de)
{
    int i;
    static const int data_sig[8] = {
        CAM_DATA_IN0_IDX, CAM_DATA_IN1_IDX, CAM_DATA_IN2_IDX, CAM_DATA_IN3_IDX,
        CAM_DATA_IN4_IDX, CAM_DATA_IN5_IDX, CAM_DATA_IN6_IDX, CAM_DATA_IN7_IDX,
    };
    if (!ring) {
        ring = heap_caps_aligned_alloc(4, RING, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        desc = heap_caps_aligned_alloc(4, NODES * sizeof *desc, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        PERIPH_RCC_ACQUIRE_ATOMIC(PERIPH_LCD_CAM_MODULE, ref_count) {
            (void)ref_count;
            cam_ll_enable_bus_clock(0, true);
            cam_ll_reset_register(0);
        }
        gdma_channel_alloc_config_t alloc = { .direction = GDMA_CHANNEL_DIRECTION_RX };
        ESP_ERROR_CHECK(gdma_new_ahb_channel(&alloc, &chan));
        ESP_ERROR_CHECK(gdma_connect(chan, GDMA_MAKE_TRIGGER(GDMA_TRIG_PERIPH_CAM, 0)));
        gdma_strategy_config_t strategy = { .auto_update_desc = false, .owner_check = false };
        ESP_ERROR_CHECK(gdma_apply_strategy(chan, &strategy));
    }
    cam_ll_stop(&LCD_CAM);
    gdma_stop(chan);
    gdma_reset(chan);
    {
        gdma_transfer_config_t transfer = { .max_data_burst_size = (uint32_t)burst, .access_ext_mem = false };
        ESP_ERROR_CHECK(gdma_config_transfer(chan, &transfer));
    }
    memset(ring, 0xff, RING);
    for (i = 0; i < NODES; i++) {
        desc[i].dw0.size = NODE;
        desc[i].dw0.length = 0;
        desc[i].dw0.err_eof = 0;
        desc[i].dw0.suc_eof = 0;
        desc[i].dw0.owner = DMA_DESCRIPTOR_BUFFER_OWNER_DMA;
        desc[i].buffer = ring + i * NODE;
        desc[i].next = &desc[(i + 1) % NODES];
    }
    rd = 0;

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
    cam_ll_set_vh_de_mode(&LCD_CAM, vh_de);
    cam_ll_enable_rgb_yuv_convert(&LCD_CAM, 0);

    esp_rom_gpio_connect_in_signal(clk, CAM_PCLK_IDX, false);
    esp_rom_gpio_connect_in_signal(d0, data_sig[0], false);
    esp_rom_gpio_connect_in_signal(d1 >= 0 ? d1 : GPIO_MATRIX_CONST_ZERO_INPUT, data_sig[1], false);
    for (i = 2; i < 8; i++)
        esp_rom_gpio_connect_in_signal(GPIO_MATRIX_CONST_ZERO_INPUT, data_sig[i], false);
    esp_rom_gpio_connect_in_signal(sync_mode & 1 ? GPIO_MATRIX_CONST_ONE_INPUT : GPIO_MATRIX_CONST_ZERO_INPUT, CAM_V_SYNC_IDX, false);
    esp_rom_gpio_connect_in_signal(sync_mode & 2 ? GPIO_MATRIX_CONST_ZERO_INPUT : GPIO_MATRIX_CONST_ONE_INPUT, CAM_H_SYNC_IDX, false);
    esp_rom_gpio_connect_in_signal(sync_mode & 4 ? GPIO_MATRIX_CONST_ZERO_INPUT : GPIO_MATRIX_CONST_ONE_INPUT, CAM_H_ENABLE_IDX, false);

    ESP_ERROR_CHECK(gdma_start(chan, (intptr_t)desc));
    cam_ll_reset(&LCD_CAM);
    cam_ll_fifo_reset(&LCD_CAM);
    cam_ll_start(&LCD_CAM);
}

static void self_clock(int d);

/* Bytes in memory beyond rd, not taken. */
static uint32_t landed(void)
{
    uint32_t n = 0;
    while (n < RING && ring[(rd + n) % RING] != 0xff)
        n++;
    return n;
}

static int clock_us = 2;
static void self_clock(int d)
{
    gpio_set_level(SELF_D, d);
    esp_rom_delay_us(clock_us);
    gpio_set_level(SELF_CLK, 1);
    esp_rom_delay_us(clock_us);
    gpio_set_level(SELF_CLK, 0);
    esp_rom_delay_us(clock_us);
}

static void self_test(int burst, int vh_de)
{
    int i, n, last = 0, bad = 0;
    char line[400];
    int at = 0;
    capture_start(SELF_CLK, SELF_D, -1, burst, vh_de);
    if (sync_mode & 8) {
        /* A pulse of the vertical sync, the clock running. */
        for (i = 0; i < 4; i++)
            self_clock(1);
        esp_rom_gpio_connect_in_signal(sync_mode & 1 ? GPIO_MATRIX_CONST_ZERO_INPUT : GPIO_MATRIX_CONST_ONE_INPUT, CAM_V_SYNC_IDX, false);
        for (i = 0; i < 4; i++)
            self_clock(1);
        esp_rom_gpio_connect_in_signal(sync_mode & 1 ? GPIO_MATRIX_CONST_ONE_INPUT : GPIO_MATRIX_CONST_ZERO_INPUT, CAM_V_SYNC_IDX, false);
        for (i = 0; i < 4; i++)
            self_clock(1);
        rd = landed();
    }
    for (i = 1; i <= 40; i++) {
        self_clock(i % 3 == 0);
        esp_rom_delay_us(50);
        n = (int)landed();
        if (n != last)
            at += snprintf(line + at, sizeof line - (size_t)at, " %d:%d", i, n);
        last = n;
    }
    printf("self test, sync %d, burst %d, vh_de %d: clocks:bytes in memory%s\n", sync_mode, burst, vh_de, at ? line : " none");
    for (i = 0; i < last; i++)
        if (ring[(rd + i) % RING] != ((i + 1) % 3 == 0))
            bad++;
    {
        int g = 0, c = 0;
        gdma_get_group_channel_id(chan, &g, &c);
        printf("  clock pad reads %d; channel %d: fifo %08lx state %08lx link %08lx dscr %08lx; cam ctrl %08lx ctrl1 %08lx int %08lx\n",
            gpio_get_level(SELF_CLK), c, (unsigned long)GDMA.channel[c].in.infifo_status.val, (unsigned long)GDMA.channel[c].in.state.val,
            (unsigned long)GDMA.channel[c].in.link.val, (unsigned long)GDMA.channel[c].in.dscr,
            (unsigned long)LCD_CAM.cam_ctrl.val, (unsigned long)LCD_CAM.cam_ctrl1.val, (unsigned long)LCD_CAM.lc_dma_int_raw.val);
    }
    printf("  %d bytes, %d not as sent; before %lu; first: %02x %02x %02x %02x %02x %02x\n", last, bad, (unsigned long)rd,
        ring[rd], ring[rd + 1], ring[rd + 2], ring[rd + 3], ring[rd + 4], ring[rd + 5]);
    if (!last)
        return;
    /* The ring, twice around, at three speeds. */
    {
        int pass;
        for (pass = 0; pass < 3; pass++) {
            uint32_t got = 0, wrong = 0, expect = last % 3;
            int64_t t0 = esp_timer_get_time(), t1;
            n = (int)landed();
            got = 0;
            while (n--) {
                ring[rd] = 0xff;
                rd = (rd + 1) % RING;
            }
            (void)expect;
            for (i = 0; i < 2 * RING; i++) {
                gpio_set_level(SELF_D, i & 1);
                gpio_set_level(SELF_CLK, 1);
                if (pass)
                    esp_rom_delay_us(pass == 1 ? 1 : 5);
                gpio_set_level(SELF_CLK, 0);
                if (pass)
                    esp_rom_delay_us(pass == 1 ? 1 : 5);
                if ((i & 255) == 255) {
                    while (ring[rd] != 0xff) {
                        if (ring[rd] != (got & 1))
                            wrong++;
                        got++;
                        ring[rd] = 0xff;
                        rd = (rd + 1) % RING;
                    }
                }
            }
            t1 = esp_timer_get_time();
            esp_rom_delay_us(100);
            while (ring[rd] != 0xff) {
                if (ring[rd] != (got & 1))
                    wrong++;
                got++;
                ring[rd] = 0xff;
                rd = (rd + 1) % RING;
            }
            printf("  %d clocks in %lld us: %lu bytes, %lu wrong, at %lu\n", 2 * RING, (long long)(t1 - t0),
                (unsigned long)got, (unsigned long)wrong, (unsigned long)rd);
        }
    }
}

/* The response's way out: SPI3 as a slave, its clock the spare pad's,
 * its select line tied high or low from here, its output on the other
 * spare pad (pulled down, so that a line let go reads 0), which the
 * camera reads at each rising edge. */
#define SELF_Q 6
static void cam_kick(void)
{
    int i;
    for (i = 0; i < 12; i++) {
        if (i == 4)
            esp_rom_gpio_connect_in_signal(GPIO_MATRIX_CONST_ONE_INPUT, CAM_V_SYNC_IDX, false);
        if (i == 8)
            esp_rom_gpio_connect_in_signal(GPIO_MATRIX_CONST_ZERO_INPUT, CAM_V_SYNC_IDX, false);
        self_clock(1);
    }
}

static void spi_test(int mode, int pre, int pullup)
{
    spi_dev_t *hw = SPI_LL_GET_HW(SPI3_HOST);
    static const uint8_t out[16] = { 0xfe, 0xa5, 0x3c, 0x81, 0xff, 0x00, 0xf0, 0x55, 0xaa, 0x0f, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    char line[200];
    int i, at = 0;
    uint32_t n;
    sync_mode = 0;
    capture_start(SELF_CLK, SELF_Q, -1, 0, 0);
    cam_kick();
    PERIPH_RCC_ATOMIC() {
        spi_ll_enable_bus_clock(SPI3_HOST, true);
        spi_ll_reset_register(SPI3_HOST);
    }
    spi_ll_enable_clock(SPI3_HOST, true);
    esp_rom_gpio_connect_in_signal(GPIO_MATRIX_CONST_ONE_INPUT, SPI3_CS0_IN_IDX, false);
    esp_rom_gpio_connect_in_signal(SELF_CLK, SPI3_CLK_IN_IDX, false);
    gpio_func_sel(SELF_Q, PIN_FUNC_GPIO);
    gpio_set_direction(SELF_Q, GPIO_MODE_INPUT_OUTPUT);
    gpio_set_pull_mode(SELF_Q, pullup ? GPIO_PULLUP_ONLY : GPIO_PULLDOWN_ONLY);
    esp_rom_gpio_connect_out_signal(SELF_Q, SPI3_Q_OUT_IDX, false, false);
    spi_ll_slave_init(hw);
    spi_ll_set_rx_lsbfirst(hw, 0);
    spi_ll_set_tx_lsbfirst(hw, 0);
    spi_ll_slave_set_mode(hw, mode, false);
    spi_ll_slave_reset(hw);
    spi_ll_write_buffer(hw, out, 128);
    spi_ll_cpu_tx_fifo_reset(hw);
    spi_ll_slave_set_rx_bitlen(hw, 128);
    spi_ll_slave_set_tx_bitlen(hw, 128);
    spi_ll_enable_mosi(hw, 1);
    spi_ll_enable_miso(hw, 1);
    spi_ll_apply_config(hw);
    for (i = 0; i < 16; i++) {
        if (i == 16 - pre)
            spi_ll_user_start(hw);
        self_clock(1);
    }
    esp_rom_delay_us(50);
    n = landed();
    while (n--) {
        ring[rd] = 0xff;
        rd = (rd + 1) % RING;
    }
    if (!pre)
        spi_ll_user_start(hw);
    /* From here a byte for each clock: 4 not selected, 48 selected, 12 not. */
    for (i = 0; i < 64; i++) {
        if (i == 4)
            esp_rom_gpio_connect_in_signal(GPIO_MATRIX_CONST_ZERO_INPUT, SPI3_CS0_IN_IDX, false);
        if (i == 52)
            esp_rom_gpio_connect_in_signal(GPIO_MATRIX_CONST_ONE_INPUT, SPI3_CS0_IN_IDX, false);
        self_clock(1);
    }
    esp_rom_delay_us(50);
    for (i = 0; i < 64 && ring[(rd + i) % RING] != 0xff; i++) {
        if (i == 4 || i == 52)
            line[at++] = ' ';
        line[at++] = (char)('0' + (ring[(rd + i) % RING] & 1));
    }
    line[at] = 0;
    printf("SPI slave out, mode %d, %d clocks before, pull-%s, %d us: %s\n", mode, pre, pullup ? "up" : "down", clock_us, line);
}

/* The slot. */
static uint32_t clocks, commands, bad_crc, dat0_low, lines_out;
static int collecting;
static uint8_t frame[6];
static pcnt_unit_handle_t pcnt;

static void command_done(void)
{
    int ok = crc7(frame, 5) == frame[5] >> 1;
    commands++;
    if (!ok)
        bad_crc++;
    if (lines_out < 40) {
        lines_out++;
        printf("%s%d %02x%02x%02x%02x%s%s\n", frame[0] & 0x40 ? "CMD" : "response ", frame[0] & 0x3f,
            frame[1], frame[2], frame[3], frame[4], ok ? "" : "  bad CRC", frame[5] & 1 ? "" : "  no end bit");
    }
}

static void take(void)
{
    for (;;) {
        uint8_t b = ring[rd];
        if (b == 0xff)
            return;
        ring[rd] = 0xff;
        rd = (rd + 1) % RING;
        clocks++;
        if (!(b & 2))
            dat0_low++;
        if (!collecting) {
            if (b & 1)
                continue;
            collecting = 1;
            memset(frame, 0, sizeof frame);
            continue;       /* the start bit, frame's bit 47, is 0 */
        }
        if (b & 1)
            frame[collecting / 8] |= (uint8_t)(0x80 >> (collecting % 8));
        if (++collecting == 48) {
            collecting = 0;
            command_done();
        }
    }
}

void app_main(void)
{
    int64_t next;
    input_pad(PIN_CMD);
    input_pad(PIN_CLK);
    input_pad(PIN_DAT0);
    crc_init();
    printf("\nprobe: CMD GPIO %d, CLK GPIO %d, DAT0 GPIO %d, all inputs\n", PIN_CMD, PIN_CLK, PIN_DAT0);

    gpio_func_sel(SELF_CLK, PIN_FUNC_GPIO);
    gpio_func_sel(SELF_D, PIN_FUNC_GPIO);
    gpio_set_direction(SELF_CLK, GPIO_MODE_INPUT_OUTPUT);
    gpio_set_direction(SELF_D, GPIO_MODE_INPUT_OUTPUT);
    gpio_set_level(SELF_CLK, 0);
    sync_mode = 8;
    self_test(0, 0);
    {
        int m;
        for (m = 0; m < 4; m++) {
            if (m == 2)
                clock_us = 20;
            spi_test(m & 1, 0, 0);
            spi_test(m & 1, 3, 0);
            spi_test(m & 1, 8, 0);
            spi_test(m & 1, 8, 1);
            clock_us = 2;
        }
    }
    sync_mode = 0;

    {
        pcnt_unit_config_t unit = { .low_limit = -1, .high_limit = 20000, .flags.accum_count = 1 };
        pcnt_chan_config_t ch = { .edge_gpio_num = PIN_CLK, .level_gpio_num = -1 };
        pcnt_channel_handle_t c;
        ESP_ERROR_CHECK(pcnt_new_unit(&unit, &pcnt));
        ESP_ERROR_CHECK(pcnt_new_channel(pcnt, &ch, &c));
        ESP_ERROR_CHECK(pcnt_channel_set_edge_action(c, PCNT_CHANNEL_EDGE_ACTION_INCREASE, PCNT_CHANNEL_EDGE_ACTION_HOLD));
        ESP_ERROR_CHECK(pcnt_unit_add_watch_point(pcnt, 20000));
        ESP_ERROR_CHECK(pcnt_unit_enable(pcnt));
        ESP_ERROR_CHECK(pcnt_unit_clear_count(pcnt));
        ESP_ERROR_CHECK(pcnt_unit_start(pcnt));
        gpio_set_pull_mode(PIN_CLK, GPIO_FLOATING);
    }
    /* The camera starts at a pulse of its vertical sync, which it
     * reads by the pixel clock: given on the spare pad, and the slot's
     * clock put in its place after. */
    sync_mode = 0;
    capture_start(SELF_CLK, PIN_CMD, PIN_DAT0, 0, 0);
    {
        int i;
        for (i = 0; i < 12; i++) {
            if (i == 4)
                esp_rom_gpio_connect_in_signal(GPIO_MATRIX_CONST_ONE_INPUT, CAM_V_SYNC_IDX, false);
            if (i == 8)
                esp_rom_gpio_connect_in_signal(GPIO_MATRIX_CONST_ZERO_INPUT, CAM_V_SYNC_IDX, false);
            self_clock(1);
        }
        esp_rom_gpio_connect_in_signal(PIN_CLK, CAM_PCLK_IDX, false);
    }
    printf("listening\n");
    next = esp_timer_get_time() + 1000000;
    for (;;) {
        take();
        if (esp_timer_get_time() >= next) {
            int count = 0;
            next += 1000000;
            pcnt_unit_get_count(pcnt, &count);
            pcnt_unit_clear_count(pcnt);
            printf("levels CLK %d CMD %d DAT0 %d  clock edges %d (counter) %lu (captured)  commands %lu  bad CRC %lu  DAT0 low %lu\n",
                gpio_get_level(PIN_CLK), gpio_get_level(PIN_CMD), gpio_get_level(PIN_DAT0), count,
                (unsigned long)clocks, (unsigned long)commands, (unsigned long)bad_crc, (unsigned long)dat0_low);
            clocks = commands = bad_crc = dat0_low = 0;
            lines_out = 0;
            vTaskDelay(1);
        }
    }
}
