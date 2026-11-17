/* The handshake desk test: as sdphone.c, the RP2040 the USB host of an
 * Android phone in accessory mode, and after Android Auto's version
 * exchange its TLS handshake (src/aa_tls.c, mbedTLS), the Pico the
 * client with the certificate the build was given (AA_CERT, AA_KEY:
 * docs/sdbridge.md).  Then "authentication complete" and the phone's
 * first encrypted message, its service discovery request, whose text
 * fields go to the log.
 *
 * With a self-signed test certificate the phone ends the handshake with
 * an alert once it has seen the certificate: the log says which, and
 * how long each step took on the Pico.  That is the expected result of
 * this test with a stand-in certificate.
 *
 * The LED (src/status.h): WAIT for a phone, BUSY while it switches and
 * the handshake runs, WORKING once the handshake is done, OK when the
 * service discovery request has been read, FAIL for an alert or any
 * other failure (until the phone is unplugged).  The log is on GP0
 * (UART0 TX, 115200 baud).
 *
 * TinyUSB and TLS run on core 1, which has a stack of 16 KiB here (the
 * key operations want more than core 0's 2 KiB); core 0 keeps the LED.
 * Powered as sdphone.c is: 5 V on VBUS. */
#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "tusb.h"
#include "aap.h"
#include "aoa.h"
#include "aa_tls.h"
#include "status.h"
#include "mbedtls/error.h"
#include "hu_cert.h"

static struct aap_reader reader;
static struct aap_assembler msg;
static struct aa_tls tls;
static uint8_t tls_out[AA_TLS_BUF];
static uint8_t frame[AA_TLS_BUF + 8];
static uint8_t plain[AAP_FRAME_MAX];

enum { WAITING, SWITCHING, VERSION, HANDSHAKE, AUTHED, DISCOVERED, FAILED };
static int state = WAITING;
static uint8_t daddr;

static void fail(const char *why, int r)
{
    char text[100];
    if (r) {
        mbedtls_strerror(r, text, sizeof text);
        printf("%s: %s (-0x%04x)\n", why, text, (unsigned)-r);
    } else {
        printf("%s\n", why);
    }
    state = FAILED;
    status_set(ST_FAIL);
}

static uint32_t ms(void)
{
    return to_ms_since_boot(get_absolute_time());
}

/* What mbedTLS wrote, as a handshake message: built in `frame`. */
static int send_handshake(const uint8_t *p, size_t n)
{
    size_t i, len = n + 2;
    if (!n)
        return 0;
    frame[0] = 0;
    frame[1] = AAP_FIRST | AAP_LAST;
    frame[2] = (uint8_t)(len >> 8);
    frame[3] = (uint8_t)len;
    frame[4] = 0x00;
    frame[5] = AAP_SSL_HANDSHAKE;
    for (i = 0; i < n; i++)
        frame[6 + i] = p[i];
    printf("handshake: %u bytes to the phone\n", (unsigned)n);
    return aoa_send(daddr, frame, (uint32_t)(len + 4));
}

/* A step of the handshake, with the bytes the phone sent (or none). */
static void handshake(const uint8_t *in, size_t n)
{
    size_t out_len;
    uint32_t t0 = ms();
    int r = aa_tls_handshake(&tls, in, n, tls_out, &out_len);
    printf("handshake: %u bytes from the phone, %lu ms on the Pico\n", (unsigned)n, (unsigned long)(ms() - t0));
    if (out_len && send_handshake(tls_out, out_len)) {
        fail("sending to the phone failed", 0);
        return;
    }
    if (r == MBEDTLS_ERR_SSL_FATAL_ALERT_MESSAGE) {
        int a = aa_tls_alert(&tls);
        printf("the phone ended the handshake with alert %d%s\n", a,
            a == 42 ? " (bad certificate)" : a == 48 ? " (unknown CA)" : a == 46 ? " (certificate unknown)" :
            a == 40 ? " (handshake failure)" : a == 43 ? " (unsupported certificate)" : "");
        printf("with a test certificate, that is the expected end of this test\n");
        state = FAILED;
        status_set(ST_FAIL);
        return;
    }
    if (r < 0) {
        fail("handshake", r);
        return;
    }
    if (r == 1) {
        uint8_t payload[8];
        int k = aap_auth_complete(payload);
        printf("handshake done: %s\n", aa_tls_suite(&tls));
        if (aoa_send(daddr, frame, (uint32_t)aap_frame(frame, 0, 0, payload, (uint16_t)k))) {
            fail("sending to the phone failed", 0);
            return;
        }
        printf("authentication complete sent\n");
        state = AUTHED;
        status_set(ST_WORKING);
    }
}

/* The text in a protocol buffer, a line a field (one level down too). */
static void show_strings(const uint8_t *p, uint32_t n, int depth)
{
    struct aap_pb_field f;
    uint32_t at = 0, i;
    while (aap_pb_next(p, n, &at, &f) == 1) {
        if (f.type == 0) {
            printf("%*sfield %lu = %llu\n", 2 * depth + 2, "", (unsigned long)f.number, (unsigned long long)f.value);
        } else if (f.type == 2) {
            int text = f.len > 0;
            for (i = 0; i < f.len; i++)
                if (f.bytes[i] < 0x20 || f.bytes[i] > 0x7e)
                    text = 0;
            if (text) {
                printf("%*sfield %lu = \"%.*s\"\n", 2 * depth + 2, "", (unsigned long)f.number, (int)f.len, f.bytes);
            } else {
                printf("%*sfield %lu: %lu bytes\n", 2 * depth + 2, "", (unsigned long)f.number, (unsigned long)f.len);
                if (depth < 1)
                    show_strings(f.bytes, f.len, depth + 1);
            }
        }
    }
}

static void message(const uint8_t *m, uint32_t n, int channel)
{
    uint16_t id;
    if (n < 2)
        return;
    id = (uint16_t)(m[0] << 8 | m[1]);
    printf("message %04x on channel %d, %lu bytes\n", id, channel, (unsigned long)n);
    if (channel == 0 && id == AAP_SERVICE_DISCOVERY_REQUEST) {
        printf("the phone's service discovery request:\n");
        show_strings(m + 2, n - 2, 0);
        state = DISCOVERED;
        status_set(ST_OK);
    }
}

static void frames(void)
{
    struct aap_frame f;
    uint16_t major, minor, st;
    while (state != FAILED && aap_next(&reader, &f)) {
        if (state == VERSION) {
            if (aap_version_response(&f, &major, &minor, &st) == 0) {
                printf("Android Auto answered: version %u.%u, %s\n", major, minor, st == 0 ? "they match" : "they do not match");
                if (st != 0) {
                    fail("version mismatch", 0);
                    return;
                }
                state = HANDSHAKE;
                status_set(ST_BUSY);
                if (aa_tls_init(&tls, 0, hu_cert_pem, sizeof hu_cert_pem, hu_key_pem, sizeof hu_key_pem, NULL, 0)) {
                    fail("the certificate or key the build was given did not parse", 0);
                    return;
                }
                handshake(NULL, 0);
            }
            continue;
        }
        if (f.flags & AAP_ENCRYPTED) {
            size_t n;
            int r = aa_tls_decrypt(&tls, f.payload, f.length, plain, sizeof plain, &n);
            if (r) {
                fail("decrypting", r);
                return;
            }
            if (aap_assemble(&msg, f.flags, plain, (uint32_t)n) == 1)
                message(msg.buf, msg.len, f.channel);
            continue;
        }
        if (aap_assemble(&msg, f.flags, f.payload, f.length) != 1)
            continue;
        if (state == HANDSHAKE && f.channel == 0 && msg.len >= 2 && msg.buf[0] == 0 && msg.buf[1] == AAP_SSL_HANDSHAKE)
            handshake(msg.buf + 2, msg.len - 2);
        else
            message(msg.buf, msg.len, f.channel);
    }
}

static void core1(void)
{
    tuh_init(0);
    for (;;) {
        tuh_task();
        if (!aoa_device && state != WAITING && state != SWITCHING) {
            if (state >= HANDSHAKE)
                aa_tls_free(&tls);
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
                uint8_t m[16];
                int n = aap_version_request(m);
                aap_reader_init(&reader);
                aap_assembler_init(&msg);
                if (aoa_open(daddr) || aoa_send(daddr, m, (uint32_t)n) || aoa_read(daddr)) {
                    fail("the accessory's endpoints did not work", 0);
                } else {
                    printf("version request sent\n");
                    state = VERSION;
                    status_set(ST_BUSY);
                }
            } else {
                status_set(ST_BUSY);
                if (aoa_switch(daddr))
                    fail("no accessory mode", 0);
                else
                    state = SWITCHING;
            }
        }
        if (state >= VERSION && state != FAILED && aoa_in_done) {
            if (aoa_in_done < 0) {
                fail("reading from the phone failed", 0);
                continue;
            }
            if (aap_feed(&reader, aoa_in, aoa_in_len)) {
                fail("the phone's bytes make no frames", 0);
                continue;
            }
            frames();
            if (state != FAILED && aoa_read(daddr))
                fail("reading from the phone failed", 0);
        }
    }
}

static uint32_t core1_stack[16384 / 4];

int main(void)
{
    stdio_init_all();
    status_init(pio1, 0);
    printf("\nsdhandshake: waiting for a phone on the USB port (a test certificate: expect the phone to refuse it)\n");
    multicore_launch_core1_with_stack(core1, core1_stack, sizeof core1_stack);
    for (;;) {
        status_tick();
        sleep_ms(5);
    }
}
