#include <stdio.h>
#include <string.h>
#include "tusb.h"
#include "aoa.h"

#define AOA_VID 0x18d1
#define AOA_PID 0x2d00
#define AOA_PID_ADB 0x2d01

/* The accessory's strings, as Android Auto head units send them. */
static const char *const strings[] = {
    "Android",                /* manufacturer */
    "Android Auto",           /* model */
    "Android Auto",           /* description */
    "2.0.1",                  /* version */
    "https://www.android.com/auto/",
    "HU-AAAAAA001",           /* serial */
};

volatile uint8_t aoa_device;
volatile int aoa_fresh;

void tuh_mount_cb(uint8_t daddr)
{
    aoa_device = daddr;
    aoa_fresh = 1;
}

void tuh_umount_cb(uint8_t daddr)
{
    if (daddr == aoa_device)
        aoa_device = 0;
    printf("device %u gone\n", daddr);
}

int aoa_is_accessory(uint16_t vid, uint16_t pid)
{
    return vid == AOA_VID && (pid == AOA_PID || pid == AOA_PID_ADB);
}

/* A vendor request on endpoint 0, waited for. */
static int control(uint8_t daddr, uint8_t type, uint8_t request, uint16_t value, uint16_t index, void *data, uint16_t len)
{
    tusb_control_request_t setup = {
        .bmRequestType = type,
        .bRequest = request,
        .wValue = value,
        .wIndex = index,
        .wLength = len,
    };
    tuh_xfer_t x = {
        .daddr = daddr,
        .ep_addr = 0,
        .setup = &setup,
        .buffer = data,
        .complete_cb = NULL,
    };
    return tuh_control_xfer(&x) && x.result == XFER_RESULT_SUCCESS ? 0 : -1;
}

int aoa_switch(uint8_t daddr)
{
    uint8_t v[2] = { 0, 0 };
    uint16_t version;
    unsigned i;
    if (control(daddr, 0xc0, 51, 0, 0, v, 2)) {
        printf("no answer to the accessory protocol's request: not an Android phone in a mode that has it\n");
        return -1;
    }
    version = (uint16_t)(v[0] | v[1] << 8);
    printf("accessory protocol version %u\n", version);
    if (version < 1)
        return -1;
    for (i = 0; i < sizeof strings / sizeof strings[0]; i++)
        if (control(daddr, 0x40, 52, 0, (uint16_t)i, (void *)strings[i], (uint16_t)(strlen(strings[i]) + 1))) {
            printf("string %u refused\n", i);
            return -1;
        }
    if (control(daddr, 0x40, 53, 0, 0, NULL, 0)) {
        printf("start refused\n");
        return -1;
    }
    printf("asked to start accessory mode: the phone should come back as %04x:%04x\n", AOA_VID, AOA_PID);
    return 0;
}

static uint8_t ep_in, ep_out;
uint8_t aoa_in[AOA_IN_SIZE] __attribute__((aligned(4)));
volatile int aoa_in_done;
volatile uint32_t aoa_in_len;
static volatile int out_done;

int aoa_open(uint8_t daddr)
{
    static uint8_t cfg[512] __attribute__((aligned(4)));
    uint16_t total;
    uint8_t *p, *end;
    if (tuh_descriptor_get_configuration_sync(daddr, 0, cfg, sizeof cfg) != XFER_RESULT_SUCCESS) {
        printf("no configuration descriptor\n");
        return -1;
    }
    total = (uint16_t)(cfg[2] | cfg[3] << 8);
    if (total > sizeof cfg)
        total = sizeof cfg;
    ep_in = ep_out = 0;
    /* The first interface is the accessory's (ADB, when there, comes
     * after it): its first bulk endpoint each way. */
    for (p = cfg, end = cfg + total; p + 2 <= end && p[0]; p += p[0]) {
        if (p[1] == TUSB_DESC_INTERFACE && p[2] != 0)
            break;
        if (p[1] == TUSB_DESC_ENDPOINT && (p[3] & 3) == TUSB_XFER_BULK) {
            tusb_desc_endpoint_t const *e = (tusb_desc_endpoint_t const *)p;
            if ((e->bEndpointAddress & 0x80) && !ep_in) {
                ep_in = e->bEndpointAddress;
                tuh_edpt_open(daddr, e);
            } else if (!(e->bEndpointAddress & 0x80) && !ep_out) {
                ep_out = e->bEndpointAddress;
                tuh_edpt_open(daddr, e);
            }
        }
    }
    if (!ep_in || !ep_out) {
        printf("no bulk endpoints in the accessory's interface\n");
        return -1;
    }
    printf("accessory endpoints: in %02x, out %02x\n", ep_in, ep_out);
    return 0;
}

static void in_cb(tuh_xfer_t *x)
{
    aoa_in_len = x->result == XFER_RESULT_SUCCESS ? x->actual_len : 0;
    aoa_in_done = x->result == XFER_RESULT_SUCCESS ? 1 : -1;
}

static void out_cb(tuh_xfer_t *x)
{
    out_done = x->result == XFER_RESULT_SUCCESS ? 1 : -1;
}

int aoa_read(uint8_t daddr)
{
    tuh_xfer_t x = {
        .daddr = daddr,
        .ep_addr = ep_in,
        .buflen = sizeof aoa_in,
        .buffer = aoa_in,
        .complete_cb = in_cb,
    };
    aoa_in_done = 0;
    return tuh_edpt_xfer(&x) ? 0 : -1;
}

int aoa_send(uint8_t daddr, const uint8_t *p, uint32_t n)
{
    /* Sent from where it is: this waits until it has gone. */
    tuh_xfer_t x = {
        .daddr = daddr,
        .ep_addr = ep_out,
        .buflen = n,
        .buffer = (uint8_t *)p,
        .complete_cb = out_cb,
    };
    if (n > 0xffff)
        return -1;
    out_done = 0;
    if (!tuh_edpt_xfer(&x))
        return -1;
    while (!out_done && aoa_device == daddr)
        tuh_task();
    return out_done > 0 ? 0 : -1;
}
