/* TinyUSB for sdphone.c: the RP2040's own USB port as a host, one
 * device (the phone), no hub, the endpoints driven directly. */
#ifndef TUSB_CONFIG_H
#define TUSB_CONFIG_H

#ifndef CFG_TUSB_MCU
#define CFG_TUSB_MCU OPT_MCU_RP2040
#endif
#define CFG_TUSB_OS OPT_OS_PICO
#define CFG_TUSB_RHPORT0_MODE OPT_MODE_HOST
#define CFG_TUH_ENABLED 1
#define CFG_TUH_MAX_SPEED OPT_MODE_FULL_SPEED
/* A phone's configuration descriptor (MTP, ADB, ...) fits. */
#define CFG_TUH_ENUMERATION_BUFSIZE 512
#define CFG_TUH_HUB 0
#define CFG_TUH_DEVICE_MAX 1
#define CFG_TUH_ENDPOINT_MAX 8
#define CFG_TUH_API_EDPT_XFER 1

#endif
