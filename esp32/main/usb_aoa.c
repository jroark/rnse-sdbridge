#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "usb/usb_host.h"
#include "usb/usb_helpers.h"
#include "usb_aoa.h"

#define AOA_VID 0x18d1
#define AOA_PID 0x2d00
#define AOA_PID_ADB 0x2d01
#define OUT_SIZE 8192

/* The accessory's strings, as Android Auto head units send them
 * (src/aoa.c). */
static const char *const strings[] = {
    "Android",                /* manufacturer */
    "Android Auto",           /* model */
    "Android Auto",           /* description */
    "2.0.1",                  /* version */
    "https://www.android.com/auto/",
    "HU-AAAAAA001",           /* serial */
};

static usb_host_client_handle_t client;
static usb_device_handle_t dev;
static int claimed;                 /* the accessory's interface */
static uint8_t ep_in, ep_out;
static usb_transfer_t *ctrl, *in_x[2], *out_x;
static int in_next, in_flying, out_flying;
static volatile int ctrl_done, out_done;
static volatile int ev_new, ev_gone;
static volatile uint8_t ev_addr;

volatile int usb_aoa_fresh;
uint8_t *volatile usb_aoa_in;
volatile int usb_aoa_in_done;
volatile uint32_t usb_aoa_in_len;

static void lib_task(void *arg)
{
    for (;;) {
        uint32_t flags;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
    }
}

/* Called in usb_aoa_poll's task (from usb_host_client_handle_events). */
static void on_event(const usb_host_client_event_msg_t *m, void *arg)
{
    if (m->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        ev_addr = m->new_dev.address;
        ev_new = 1;
    } else if (m->event == USB_HOST_CLIENT_EVENT_DEV_GONE && m->dev_gone.dev_hdl == dev) {
        ev_gone = 1;
    }
}

static void ctrl_cb(usb_transfer_t *x)
{
    ctrl_done = 1;
}

static void in_cb(usb_transfer_t *x)
{
    in_flying = 0;
    usb_aoa_in = x->data_buffer;
    usb_aoa_in_len = x->status == USB_TRANSFER_STATUS_COMPLETED ? (uint32_t)x->actual_num_bytes : 0;
    usb_aoa_in_done = x->status == USB_TRANSFER_STATUS_COMPLETED ? 1 : -1;
}

static void out_cb(usb_transfer_t *x)
{
    out_flying = 0;
    out_done = x->status == USB_TRANSFER_STATUS_COMPLETED ? 1 : -1;
}

void usb_aoa_start(void)
{
    usb_host_config_t host = { .intr_flags = ESP_INTR_FLAG_LEVEL1 };
    usb_host_client_config_t c = {
        .is_synchronous = false,
        .max_num_event_msg = 8,
        .async = { .client_event_callback = on_event },
    };
    ESP_ERROR_CHECK(usb_host_install(&host));
    xTaskCreatePinnedToCore(lib_task, "usb_lib", 4096, NULL, 10, NULL, 0);
    ESP_ERROR_CHECK(usb_host_client_register(&c, &client));
    ESP_ERROR_CHECK(usb_host_transfer_alloc(8 + 256, 0, &ctrl));
    ESP_ERROR_CHECK(usb_host_transfer_alloc(USB_AOA_IN_SIZE, 0, &in_x[0]));
    ESP_ERROR_CHECK(usb_host_transfer_alloc(USB_AOA_IN_SIZE, 0, &in_x[1]));
    ESP_ERROR_CHECK(usb_host_transfer_alloc(OUT_SIZE, 0, &out_x));
    usb_aoa_in = in_x[0]->data_buffer;
}

/* A vendor request on endpoint 0, waited for. */
static int control(uint8_t type, uint8_t request, uint16_t value, uint16_t index, void *data, uint16_t len)
{
    usb_setup_packet_t *s = (usb_setup_packet_t *)ctrl->data_buffer;
    int in = type & 0x80, waited;
    if (len > 192)
        return -1;
    s->bmRequestType = type;
    s->bRequest = request;
    s->wValue = value;
    s->wIndex = index;
    s->wLength = len;
    if (!in && len)
        memcpy(ctrl->data_buffer + sizeof *s, data, len);
    /* (An IN data stage in whole packets: 64 covers endpoint 0's size.) */
    ctrl->num_bytes = (int)sizeof *s + (in ? (len + 63) / 64 * 64 : len);
    ctrl->device_handle = dev;
    ctrl->bEndpointAddress = 0;
    ctrl->callback = ctrl_cb;
    ctrl->timeout_ms = 1000;
    ctrl_done = 0;
    if (usb_host_transfer_submit_control(client, ctrl) != ESP_OK)
        return -1;
    for (waited = 0; !ctrl_done && waited < 300; waited++)
        usb_host_client_handle_events(client, pdMS_TO_TICKS(10));
    if (!ctrl_done || ctrl->status != USB_TRANSFER_STATUS_COMPLETED)
        return -1;
    if (in) {
        if (ctrl->actual_num_bytes < (int)(sizeof *s + len))
            return -1;
        memcpy(data, ctrl->data_buffer + sizeof *s, len);
    }
    return 0;
}

static int to_accessory(void)
{
    uint8_t v[2] = { 0, 0 };
    unsigned version, i;
    if (control(0xc0, 51, 0, 0, v, 2)) {
        printf("no answer to the accessory protocol's request: not an Android phone in a mode that has it\n");
        return -1;
    }
    version = v[0] | v[1] << 8;
    printf("accessory protocol version %u\n", version);
    if (version < 1)
        return -1;
    for (i = 0; i < sizeof strings / sizeof strings[0]; i++)
        if (control(0x40, 52, 0, (uint16_t)i, (void *)strings[i], (uint16_t)(strlen(strings[i]) + 1))) {
            printf("string %u refused\n", i);
            return -1;
        }
    if (control(0x40, 53, 0, 0, NULL, 0)) {
        printf("start refused\n");
        return -1;
    }
    printf("asked to start accessory mode: the phone should come back as %04x:%04x\n", AOA_VID, AOA_PID);
    return 0;
}

/* The accessory's interface (the first; ADB, when there, comes after
 * it): its first bulk endpoint each way. */
static int open_accessory(void)
{
    const usb_config_desc_t *cfg;
    const usb_intf_desc_t *intf;
    int offset = 0, i;
    if (usb_host_get_active_config_descriptor(dev, &cfg) != ESP_OK ||
        !(intf = usb_parse_interface_descriptor(cfg, 0, 0, &offset))) {
        printf("no configuration descriptor\n");
        return -1;
    }
    ep_in = ep_out = 0;
    for (i = 0; i < intf->bNumEndpoints; i++) {
        int at = offset;
        const usb_ep_desc_t *e = usb_parse_endpoint_descriptor_by_index(intf, i, cfg->wTotalLength, &at);
        if (!e || (e->bmAttributes & 3) != 2)
            continue;
        if ((e->bEndpointAddress & 0x80) && !ep_in)
            ep_in = e->bEndpointAddress;
        else if (!(e->bEndpointAddress & 0x80) && !ep_out)
            ep_out = e->bEndpointAddress;
    }
    if (!ep_in || !ep_out) {
        printf("no bulk endpoints in the accessory's interface\n");
        return -1;
    }
    if (usb_host_interface_claim(client, dev, 0, 0) != ESP_OK) {
        printf("the accessory's interface could not be claimed\n");
        return -1;
    }
    claimed = 1;
    printf("accessory endpoints: in %02x, out %02x\n", ep_in, ep_out);
    return 0;
}

/* The device let go, its transfers first. */
static void let_go(void)
{
    int waited;
    if (!dev)
        return;
    if (claimed) {
        if (in_flying || out_flying) {
            usb_host_endpoint_halt(dev, ep_in);
            usb_host_endpoint_flush(dev, ep_in);
            usb_host_endpoint_halt(dev, ep_out);
            usb_host_endpoint_flush(dev, ep_out);
        }
        for (waited = 0; (in_flying || out_flying) && waited < 100; waited++)
            usb_host_client_handle_events(client, pdMS_TO_TICKS(10));
        usb_host_interface_release(client, dev, 0);
    }
    claimed = 0;
    usb_host_device_close(client, dev);
    dev = NULL;
    usb_aoa_in_done = 0;
    usb_aoa_fresh = 0;
}

void usb_aoa_poll(TickType_t wait)
{
    usb_host_client_handle_events(client, wait);
    if (ev_gone) {
        ev_gone = 0;
        printf("the USB device is gone\n");
        let_go();
    }
    if (ev_new) {
        const usb_device_desc_t *d;
        ev_new = 0;
        let_go();
        if (usb_host_device_open(client, ev_addr, &dev) != ESP_OK) {
            dev = NULL;
            return;
        }
        if (usb_host_get_device_descriptor(dev, &d) != ESP_OK) {
            let_go();
            return;
        }
        printf("device %u: %04x:%04x\n", ev_addr, d->idVendor, d->idProduct);
        if (d->idVendor == AOA_VID && (d->idProduct == AOA_PID || d->idProduct == AOA_PID_ADB)) {
            if (open_accessory() == 0)
                usb_aoa_fresh = 1;
        } else {
            /* It leaves the bus and comes back as an accessory. */
            to_accessory();
        }
    }
}

int usb_aoa_up(void)
{
    return dev && claimed;
}

int usb_aoa_read(void)
{
    usb_transfer_t *x = in_x[in_next];
    if (!usb_aoa_up() || in_flying)
        return -1;
    x->device_handle = dev;
    x->bEndpointAddress = ep_in;
    x->num_bytes = USB_AOA_IN_SIZE;
    x->callback = in_cb;
    x->timeout_ms = 0;
    usb_aoa_in_done = 0;
    in_flying = 1;
    if (usb_host_transfer_submit(x) != ESP_OK) {
        in_flying = 0;
        return -1;
    }
    in_next ^= 1;
    return 0;
}

int usb_aoa_send(const uint8_t *p, uint32_t n)
{
    while (n) {
        uint32_t k = n < OUT_SIZE ? n : OUT_SIZE;
        int waited;
        if (!usb_aoa_up() || ev_gone)
            return -1;
        memcpy(out_x->data_buffer, p, k);
        out_x->device_handle = dev;
        out_x->bEndpointAddress = ep_out;
        out_x->num_bytes = (int)k;
        out_x->callback = out_cb;
        out_x->timeout_ms = 0;
        out_done = 0;
        out_flying = 1;
        if (usb_host_transfer_submit(out_x) != ESP_OK) {
            out_flying = 0;
            return -1;
        }
        for (waited = 0; !out_done && !ev_gone && waited < 2000; waited++)
            usb_host_client_handle_events(client, pdMS_TO_TICKS(1));
        if (out_done <= 0)
            return -1;
        p += k;
        n -= k;
    }
    return 0;
}
