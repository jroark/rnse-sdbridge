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
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "tusb.h"
#include "aap.h"
#include "aoa.h"
#include "status.h"

static struct aap_reader reader;

static int send_version_request(uint8_t daddr)
{
    uint8_t m[16];
    int n = aap_version_request(m);
    return aoa_send(daddr, m, (uint32_t)n);
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
        if (!aoa_device && state != WAITING && state != SWITCHING) {
            state = WAITING;
            status_set(ST_WAIT);
        }
        if (aoa_fresh) {
            uint16_t vid = 0, pid = 0;
            aoa_fresh = 0;
            daddr = aoa_device;
            tuh_vid_pid_get(daddr, &vid, &pid);
            printf("device %u: %04x:%04x\n", daddr, vid, pid);
            if (aoa_is_accessory(vid, pid)) {
                aap_reader_init(&reader);
                if (aoa_open(daddr) || send_version_request(daddr) || aoa_read(daddr)) {
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
                state = aoa_switch(daddr) ? FAILED : SWITCHING;
                if (state == FAILED)
                    status_set(ST_FAIL);
            }
        }
        if (state == ACCESSORY && aoa_in_done) {
            struct aap_frame f;
            uint16_t major, minor, status;
            if (aoa_in_done < 0) {
                printf("reading from the phone failed\n");
                state = FAILED;
                status_set(ST_FAIL);
                continue;
            }
            if (aap_feed(&reader, aoa_in, aoa_in_len)) {
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
            if (state == ACCESSORY && aoa_read(daddr)) {
                state = FAILED;
                status_set(ST_FAIL);
            }
        }
        if (state == ACCESSORY && absolute_time_diff_us(since, get_absolute_time()) > 5000000) {
            printf("no answer in 5 s: is Android Auto installed and allowed on the phone?\n");
            since = get_absolute_time();
        }
    }
}
