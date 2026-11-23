/* The phone on the ESP32-S3's own USB port, as src/aoa.h has it on the
 * RP2040: a phone that attaches is asked into accessory mode (AOA), and
 * when it comes back as an accessory its bulk endpoints are the link.
 * On ESP-IDF's USB host library; all of it called from one task. */
#ifndef USB_AOA_H
#define USB_AOA_H
#include <stdint.h>
#include "freertos/FreeRTOS.h"

#define USB_AOA_IN_SIZE 16384

/* The host library installed and its task started; this task its client. */
void usb_aoa_start(void);
/* USB's events taken, waiting up to `wait` for one: phones attached,
 * switched, opened and let go. */
void usb_aoa_poll(TickType_t wait);
/* 1 while an accessory's endpoints are open. */
int usb_aoa_up(void);
/* Set when an accessory has just been opened; the caller clears it. */
extern volatile int usb_aoa_fresh;

/* A read started into the buffer that is not usb_aoa_in; when it is
 * done usb_aoa_in_done is 1 (-1 failed), usb_aoa_in the bytes and
 * usb_aoa_in_len their number. */
int usb_aoa_read(void);
extern uint8_t *volatile usb_aoa_in;
extern volatile int usb_aoa_in_done;
extern volatile uint32_t usb_aoa_in_len;
/* Bytes to the phone, waited for: 0, or -1. */
int usb_aoa_send(const uint8_t *p, uint32_t n);

#endif
