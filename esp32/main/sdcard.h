/* The ESP32-S3 as the SD card in the unit's slot, with no RP2040: the
 * card's commands (src/card.c) and its made-up volume (src/volume.c)
 * answered on the host's clock by LCD_CAM, two SPI slaves and a loop
 * that has the second core to itself (sdcard.c).  One data line,
 * reading only.
 *
 * Wiring, each line through 100 ohms: CMD GPIO 11, CLK GPIO 12, DAT0
 * GPIO 13, and ground.  GPIO 4 is used inside the chip (leave it
 * unconnected). */
#ifndef SDCARD_H
#define SDCARD_H
#include <stdint.h>

#define SDCARD_PIN_CMD 11
#define SDCARD_PIN_CLK 12
#define SDCARD_PIN_DAT0 13

/* Called from the card's loop, on its core, between a command and the
 * next: short, in RAM (IRAM_ATTR), touching nothing that waits.  read:
 * the host has started a read at this sector.  sent: this sector's
 * block has gone out whole. */
struct sdcard_hooks {
    void (*read)(uint32_t sector);
    void (*sent)(uint32_t sector);
};

/* The pins, the peripherals and the loop on core 1.  Call it last:
 * after it nothing else runs on that core. */
void sdcard_start(const struct sdcard_hooks *hooks);
/* A line of the log with what the card did since the last, after the
 * commands that were not reads when `commands` is set. */
void sdcard_report(int commands);

#endif
