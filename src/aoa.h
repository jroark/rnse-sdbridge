/* The phone over USB, the RP2040 its host (TinyUSB): Android Open
 * Accessory's switch, as an Android Auto head unit makes it, and the
 * accessory's two bulk endpoints.  Shared by the desk tests
 * (sdphone.c, sdhandshake.c and sdstream). */
#ifndef AOA_H
#define AOA_H
#include <stdint.h>

/* The device's address, 0 when there is none; aoa_fresh is set by each
 * new one (the caller clears it). */
extern volatile uint8_t aoa_device;
extern volatile int aoa_fresh;

/* Google's ids of a device in accessory mode (with or without ADB). */
int aoa_is_accessory(uint16_t vid, uint16_t pid);
/* Ask the device to switch: 0, after which it drops off the bus and
 * comes back as an accessory; -1 when it has no accessory mode. */
int aoa_switch(uint8_t daddr);
/* The accessory interface's bulk endpoints opened: 0, or -1. */
int aoa_open(uint8_t daddr);

/* Bytes from the phone: aoa_read starts a transfer; when it completes,
 * aoa_in_done is 1 (aoa_in_len bytes at aoa_in) or -1 (failed).  A
 * transfer ends with the phone's short packet or a full buffer, so a
 * larger AOA_IN_SIZE (sdstream: 16 KiB) takes many 64-byte packets at
 * once.  There are two buffers and each transfer takes the one the last
 * did not: a caller may start the next transfer as soon as one is done
 * and take the bytes apart (from aoa_in as it was then) while the next
 * comes in. */
#ifndef AOA_IN_SIZE
#define AOA_IN_SIZE 512
#endif
extern uint8_t *volatile aoa_in;
extern volatile int aoa_in_done;
extern volatile uint32_t aoa_in_len;
int aoa_read(uint8_t daddr);
/* Bytes to the phone, waited for (running TinyUSB meanwhile): 0, or -1. */
int aoa_send(uint8_t daddr, const uint8_t *p, uint32_t n);

#endif
