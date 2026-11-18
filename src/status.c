#include "status.h"
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "sd.pio.h"

static enum status now = ST_WAIT;

#ifdef PICO_DEFAULT_WS2812_PIN

static PIO led_pio;
static uint led_sm;

static void colour(uint8_t r, uint8_t g, uint8_t b)
{
    pio_sm_put(led_pio, led_sm, ((uint32_t)g << 24) | ((uint32_t)r << 16) | ((uint32_t)b << 8));
}

void status_init(PIO pio, uint sm)
{
    uint off = pio_add_program(pio, &ws2812_program);
    pio_sm_config c = ws2812_program_get_default_config(off);
    led_pio = pio;
    led_sm = sm;
    pio_gpio_init(pio, PICO_DEFAULT_WS2812_PIN);
    pio_sm_set_consecutive_pindirs(pio, sm, PICO_DEFAULT_WS2812_PIN, 1, true);
    sm_config_set_sideset_pins(&c, PICO_DEFAULT_WS2812_PIN);
    sm_config_set_out_shift(&c, false, true, 24);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
    sm_config_set_clkdiv(&c, (float)clock_get_hz(clk_sys) / 8000000.0f);
    pio_sm_init(pio, sm, off, &c);
    pio_sm_set_enabled(pio, sm, true);
    status_set(ST_WAIT);
}

void status_set(enum status s)
{
    static const uint8_t c[][3] = {
        { 0, 0, 32 }, { 24, 24, 0 }, { 0, 24, 24 }, { 0, 32, 0 }, { 32, 0, 32 }, { 32, 0, 0 },
    };
    now = s;
    colour(c[s][0], c[s][1], c[s][2]);
}

void status_tick(void)
{
}

#else

#ifndef PICO_DEFAULT_LED_PIN
#error "the board has no LED this knows"
#endif

/* Patterns: milliseconds on, off, on, off, ... ending with 0. */
static const uint16_t patterns[][8] = {
    [ST_WAIT] = { 100, 1900, 0 },
    [ST_BUSY] = { 250, 250, 0 },
    [ST_WORKING] = { 50, 50, 0 },
    [ST_OK] = { 1000, 0 },
    [ST_WARN] = { 150, 150, 150, 1550, 0 },
    [ST_FAIL] = { 150, 150, 150, 150, 150, 1250, 0 },
};
static uint32_t step_at;
static int step;

void status_init(PIO pio, uint sm)
{
    (void)pio;
    (void)sm;
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
    status_set(ST_WAIT);
}

void status_set(enum status s)
{
    if (s == now && step_at)
        return;
    now = s;
    step = 0;
    step_at = to_ms_since_boot(get_absolute_time());
    gpio_put(PICO_DEFAULT_LED_PIN, 1);
}

void status_tick(void)
{
    uint32_t t = to_ms_since_boot(get_absolute_time());
    const uint16_t *p = patterns[now];
    if (t - step_at < p[step])
        return;
    step_at = t;
    step++;
    if (!p[step] || (now == ST_OK))
        step = 0;
    /* Even steps on, odd ones off; OK stays on. */
    gpio_put(PICO_DEFAULT_LED_PIN, now == ST_OK || !(step & 1));
}

#endif
