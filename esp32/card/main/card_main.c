/* The ESP32-S3 as the SD card in the unit's slot, by itself: the card
 * (../../main/sdcard.c) with LINKTEST.BIN on its volume and, when the
 * build was given one (CARD_PROGRAM), a program as LINKBENC.BIN.  No
 * phone, no Wi-Fi: the test of the card alone (link193, linkbench). */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdcard.h"
#include "volume.h"

void app_main(void)
{
    extern const uint8_t program_start[] asm("_binary_card_program_bin_start");
    extern const uint8_t program_end[] asm("_binary_card_program_bin_end");
    uint32_t n = (uint32_t)(program_end - program_start);
    if (n > 16 && n <= VOLUME_PROGRAM_ROOM) {
        memcpy(volume_program, program_start, n);
        volume_program_bytes = n;
        printf("LINKBENC.BIN on the card: %lu bytes\n", (unsigned long)n);
    }
    printf("\ncard: CMD GPIO %d, CLK GPIO %d, DAT0 GPIO %d\n", SDCARD_PIN_CMD, SDCARD_PIN_CLK, SDCARD_PIN_DAT0);
    sdcard_start(NULL);
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        sdcard_report(1);
    }
}
