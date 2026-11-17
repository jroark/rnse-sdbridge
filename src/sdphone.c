/* The bridge's other side, on the desk: the RP2040's USB port as the
 * host of an Android phone, which it switches into accessory mode (AOA)
 * the way an Android Auto head unit does, and then asks for Android
 * Auto's protocol version, the first message of a session.  No unit,
 * no card, no certificate, no TLS: what this shows is that the cable,
 * the power and the phone's accessory mode work, and that the phone's
 * Android Auto answers (docs/sdbridge.md, "The phone on the desk").
 *
 * The LED says how far it got (src/status.h: a colour on an RP2040-Zero,
 * a pattern on a Raspberry Pi Pico):
 *   WAIT     waiting for a phone
 *   BUSY     a phone: asking it to switch to accessory mode
 *   WORKING  in accessory mode, the version request sent
 *   OK       Android Auto answered, the versions match (stays)
 *   WARN     it answered, the versions do not match (stays)
 *   FAIL     the device has no accessory mode, or something failed
 *            (stays until the phone is unplugged)
 * and the log on GP0 (UART0 TX, 115200 baud) says it in words.
 *
 * The board's USB port is the host here, so the board is powered with
 * 5 V on the pin that is the USB port's VBUS (a Pico's VBUS, pin 40; an
 * RP2040-Zero's 5V), and the phone takes its USB power from that too. */
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "tusb.h"
#include "aap.h"
#include "status.h"

/* Google's vendor id and the two product ids of accessory mode (the
 * second with ADB as well). */
#define AOA_VID 0x18d1
#define AOA_PID 0x2d00
#define AOA_PID_ADB 0x2d01

/* The accessory's strings, as Android Auto head units send them. */
static const char *const aoa_strings[] = {
    "Android",                /* manufacturer */
    "Android Auto",           /* model */
    "Android Auto",           /* description */
    "2.0.1",                  /* version */
    "https://www.android.com/auto/",
    "HU-AAAAAA001",           /* serial */
};

/* ---- the device ---------------------------------------------------- */

static volatile uint8_t mounted;      /* the device's address, 0 none */
static volatile int fresh;            /* mounted since last looked at */

void tuh_mount_cb(uint8_t daddr)
{
    mounted = daddr;
    fresh = 1;
}

void tuh_umount_cb(uint8_t daddr)
{
    if (daddr == mounted)
        mounted = 0;
    printf("device %u gone\n", daddr);
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

/* Into accessory mode: the protocol's version, the strings, start.  The
 * phone then drops off the bus and comes back as 18d1:2d00 or 2d01. */
static int switch_to_accessory(uint8_t daddr)
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
    for (i = 0; i < sizeof aoa_strings / sizeof aoa_strings[0]; i++)
        if (control(daddr, 0x40, 52, 0, (uint16_t)i, (void *)aoa_strings[i], (uint16_t)(strlen(aoa_strings[i]) + 1))) {
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

/* ---- accessory mode ------------------------------------------------- */

static uint8_t ep_in, ep_out;
static uint8_t in_buf[512] __attribute__((aligned(4)));
static uint8_t out_buf[64] __attribute__((aligned(4)));
static volatile int in_done, out_done;
static volatile uint32_t in_len;
static struct aap_reader reader;

static void in_cb(tuh_xfer_t *x)
{
    in_len = x->result == XFER_RESULT_SUCCESS ? x->actual_len : 0;
    in_done = x->result == XFER_RESULT_SUCCESS ? 1 : -1;
}

static void out_cb(tuh_xfer_t *x)
{
    out_done = x->result == XFER_RESULT_SUCCESS ? 1 : -1;
}

static int read_more(uint8_t daddr)
{
    tuh_xfer_t x = {
        .daddr = daddr,
        .ep_addr = ep_in,
        .buflen = sizeof in_buf,
        .buffer = in_buf,
        .complete_cb = in_cb,
    };
    in_done = 0;
    return tuh_edpt_xfer(&x) ? 0 : -1;
}

/* The accessory interface's two bulk endpoints, opened. */
static int open_accessory(uint8_t daddr)
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

static int send_version_request(uint8_t daddr)
{
    int n = aap_version_request(out_buf);
    tuh_xfer_t x = {
        .daddr = daddr,
        .ep_addr = ep_out,
        .buflen = (uint32_t)n,
        .buffer = out_buf,
        .complete_cb = out_cb,
    };
    out_done = 0;
    return tuh_edpt_xfer(&x) ? 0 : -1;
}

enum { WAITING, SWITCHING, ACCESSORY, ANSWERED, FAILED };

int main(void)
{
    int state = WAITING;
    uint8_t daddr = 0;
    absolute_time_t since = get_absolute_time();

    stdio_init_all();
    status_init(pio1, 0);
    printf("\nsdphone: waiting for a phone on the USB port\n");
    tuh_init(0);

    for (;;) {
        tuh_task();
        status_tick();
        if (!mounted && state != WAITING && state != SWITCHING) {
            state = WAITING;
            status_set(ST_WAIT);
        }
        if (fresh) {
            uint16_t vid = 0, pid = 0;
            fresh = 0;
            daddr = mounted;
            tuh_vid_pid_get(daddr, &vid, &pid);
            printf("device %u: %04x:%04x\n", daddr, vid, pid);
            if (vid == AOA_VID && (pid == AOA_PID || pid == AOA_PID_ADB)) {
                aap_reader_init(&reader);
                if (open_accessory(daddr) || send_version_request(daddr) || read_more(daddr)) {
                    state = FAILED;
                    status_set(ST_FAIL);
                } else {
                    printf("version request sent\n");
                    state = ACCESSORY;
                    since = get_absolute_time();
                    status_set(ST_WORKING);
                }
            } else {
                status_set(ST_BUSY);
                state = switch_to_accessory(daddr) ? FAILED : SWITCHING;
                if (state == FAILED)
                    status_set(ST_FAIL);
            }
        }
        if (state == ACCESSORY && in_done) {
            struct aap_frame f;
            uint16_t major, minor, status;
            if (in_done < 0) {
                printf("reading from the phone failed\n");
                state = FAILED;
                status_set(ST_FAIL);
                continue;
            }
            if (aap_feed(&reader, in_buf, in_len)) {
                printf("the phone's bytes make no frames\n");
                state = FAILED;
                status_set(ST_FAIL);
                continue;
            }
            while (aap_next(&reader, &f)) {
                printf("frame: channel %u, flags %02x, %u bytes\n", f.channel, f.flags, f.length);
                if (aap_version_response(&f, &major, &minor, &status) == 0) {
                    printf("Android Auto answered: version %u.%u, %s\n", major, minor, status == 0 ? "they match" : "they do not match");
                    state = ANSWERED;
                    if (status == 0)
                        status_set(ST_OK);
                    else
                        status_set(ST_WARN);
                }
            }
            if (state == ACCESSORY && read_more(daddr)) {
                state = FAILED;
                status_set(ST_FAIL);
            }
        }
        if (state == ACCESSORY && out_done < 0) {
            printf("sending to the phone failed\n");
            out_done = 0;
            state = FAILED;
            status_set(ST_FAIL);
        }
        if (state == ACCESSORY && absolute_time_diff_us(since, get_absolute_time()) > 5000000) {
            printf("no answer in 5 s: is Android Auto installed and allowed on the phone?\n");
            since = get_absolute_time();
        }
    }
}
