/* The board's LED, whichever board: on an RP2040-Zero a WS2812 that
 * shows a colour, on a Raspberry Pi Pico a plain LED (GP25) that shows
 * the same states as a pattern (the board header says which:
 * PICO_DEFAULT_WS2812_PIN or PICO_DEFAULT_LED_PIN).
 *
 *   state     colour    plain LED
 *   WAIT      blue      a short flash every 2 s
 *   BUSY      yellow    blinking, twice a second
 *   WORKING   cyan      flickering
 *   OK        green     on
 *   WARN      magenta   two short flashes every 2 s
 *   FAIL      red       three short flashes every 2 s */
#ifndef STATUS_H
#define STATUS_H
#include "hardware/pio.h"

enum status { ST_WAIT, ST_BUSY, ST_WORKING, ST_OK, ST_WARN, ST_FAIL };

/* The WS2812's state machine, if the board has one. */
void status_init(PIO pio, uint sm);
void status_set(enum status s);
/* Every few milliseconds, for the plain LED's patterns. */
void status_tick(void);

#endif
