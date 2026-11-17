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
 * how long each step took on the Pico.  With a certificate the phone
 * accepts, the handshake completes.
 *
 * Built with AA_STREAM=1 it is SDSTREAM.UF2, and goes on from there as
 * an 800x480 head unit (src/aa_hu.c): it answers the service discovery
 * request, opens the channels, sets up and focuses the video channel and
 * acknowledges the phone's H.264 frames and its sound, counting them.
 * Once a second the log has the video's rate (frames, kilobytes, key
 * frames), the sound's, the USB's, and the time the Pico spends
 * decrypting.  Nothing is kept: it
 * measures what the phone sends over the RP2040's full-speed USB.
 *
 * The LED (src/status.h): WAIT for a phone, BUSY while it switches and
 * the handshake runs, WORKING once the handshake is done, OK when the
 * service discovery request has been read (SDSTREAM: when video frames
 * arrive), FAIL for an alert or any other failure (until the phone is
 * unplugged).  The log is on GP0 (UART0 TX, 115200 baud).
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
#include "aa_hu.h"
#include "status.h"
#include "mbedtls/error.h"
#include "hu_cert.h"
#if AA_STREAM
/* The TinyUSB patch's count of packets lost after a short one
 * (sdbridge/patches/, double-buffered only). */
extern volatile uint32_t rp2040_host_epx_short_lost;
#ifndef PICO_USB_HOST_BULK_DOUBLE_BUFFER
#define PICO_USB_HOST_BULK_DOUBLE_BUFFER 0
#endif
#endif

#ifndef AA_STREAM
#define AA_STREAM 0
#endif
/* The video frames the phone may send ahead of the acknowledgements. */
#ifndef AA_MAX_UNACKED
#define AA_MAX_UNACKED 1
#endif

static struct aap_reader reader;
static struct aap_assembler msg;
static struct aa_tls tls;
static struct aa_hu hu;
static uint8_t tls_out[AA_TLS_BUF];
static uint8_t frame[AA_TLS_BUF + 8];
static uint8_t plain[AAP_FRAME_MAX];

enum { WAITING, SWITCHING, VERSION, HANDSHAKE, AUTHED, DISCOVERED, STREAMING, FAILED };
static int state = WAITING;
static uint8_t daddr;

/* What the stream's log counts. */
static uint64_t usb_bytes;
static uint64_t decrypt_us;
static uint32_t frames_in;
static int authed, sound_seen;
static uint32_t t_auth, t_video, t_stats;
static struct {
    uint64_t usb, video, decrypt, audio;
    uint32_t messages, idr, frames;
} last;

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

/* A message, encrypted, as one frame. */
static int send_encrypted(uint8_t channel, uint8_t flags, const uint8_t *p, uint32_t n)
{
    size_t rl;
    int r = aa_tls_encrypt(&tls, p, n, tls_out, sizeof tls_out, &rl);
    if (r) {
        fail("encrypting", r);
        return -1;
    }
    if (aoa_send(daddr, frame, (uint32_t)aap_frame(frame, channel, (uint8_t)(flags | AAP_ENCRYPTED), tls_out, (uint16_t)rl))) {
        fail("sending to the phone failed", 0);
        return -1;
    }
    return 0;
}

/* The head unit's answers, in order. */
static void flush(void)
{
    unsigned i;
    for (i = 0; i < hu.nout && state != FAILED; i++)
        send_encrypted(hu.out[i].channel, hu.out[i].flags, hu.out[i].data, hu.out[i].len);
    hu.nout = 0;
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
        printf("with a self-signed test certificate that is the expected end; one the phone accepts goes on\n");
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
        t_auth = t_stats = ms();
        authed = 1;
        /* The seconds' counts from here. */
        last.usb = usb_bytes;
        last.decrypt = decrypt_us;
        last.frames = frames_in;
        last.video = last.audio = 0;
        last.messages = last.idr = 0;
        status_set(ST_WORKING);
        if (AA_STREAM)
            aa_hu_init(&hu, AA_MAX_UNACKED);
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

/* SDSTREAM: a decrypted frame to the head unit. */
static void session_frame(const struct aap_frame *f, const uint8_t *p, uint32_t n)
{
    if (aa_hu_media_channel(f->channel) && !(f->flags & AAP_CONTROL) &&
        aa_hu_media_frame(&hu, f->channel, f->flags, p, n)) {
        if (f->channel != AA_CH_VIDEO) {
            if (!sound_seen) {
                sound_seen = 1;
                printf("sound arriving on channel %d\n", f->channel);
            }
        } else if (state != STREAMING) {
            state = STREAMING;
            t_video = ms();
            printf("video frames arriving, %lu ms after authentication\n", (unsigned long)(t_video - t_auth));
            status_set(ST_OK);
        }
        if (hu.v.config_new) {
            uint32_t i;
            hu.v.config_new = 0;
            printf("codec configuration (%lu bytes):", (unsigned long)hu.v.config_len);
            for (i = 0; i < hu.v.config_len; i++)
                printf(" %02x", hu.v.config[i]);
            printf("\nH.264 profile %d (%s), constraints %02x, level %d.%d\n", hu.v.profile,
                hu.v.profile == 66 ? "Baseline" : hu.v.profile == 77 ? "Main" : hu.v.profile == 100 ? "High" : "?",
                hu.v.constraints & 0xff, hu.v.level / 10, hu.v.level % 10);
        }
    } else if (aap_assemble(&msg, f->flags, p, n) == 1) {
        if (state == AUTHED && f->channel == 0 && msg.len >= 2 && msg.buf[1] == AAP_SERVICE_DISCOVERY_REQUEST)
            state = DISCOVERED;
        aa_hu_message(&hu, f->channel, f->flags, msg.buf, msg.len);
    }
    flush();
}

static void frames(void)
{
    struct aap_frame f;
    uint16_t major, minor, st;
    while (state != FAILED && aap_next(&reader, &f)) {
        frames_in++;
        if (state == VERSION) {
            if (aap_version_response(&f, &major, &minor, &st) == 0) {
                printf("Android Auto answered: version %u.%u, %s\n", major, minor, st == 0 ? "they match" : "they do not match");
                if (st != 0) {
                    fail("version mismatch", 0);
                    return;
                }
                state = HANDSHAKE;
                status_set(ST_BUSY);
                if (aa_tls_init(&tls, 0, hu_cert_pem, sizeof hu_cert_pem, HU_CERT_WIRE ? hu_cert_wire : NULL,
                                HU_CERT_WIRE ? sizeof hu_cert_wire : 0, hu_key_pem, sizeof hu_key_pem, NULL, 0)) {
                    fail("the certificate or key the build was given did not parse", 0);
                    return;
                }
                handshake(NULL, 0);
            }
            continue;
        }
        if (f.flags & AAP_ENCRYPTED) {
            size_t n;
            uint32_t t0 = time_us_32();
            int r = aa_tls_decrypt(&tls, f.payload, f.length, plain, sizeof plain, &n);
            decrypt_us += time_us_32() - t0;
            if (r) {
                fail("decrypting", r);
                return;
            }
            if (AA_STREAM && state >= AUTHED)
                session_frame(&f, plain, (uint32_t)n);
            else if (aap_assemble(&msg, f.flags, plain, (uint32_t)n) == 1)
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

static uint32_t short_lost(void)
{
#if AA_STREAM
    return rp2040_host_epx_short_lost;
#else
    return 0;
#endif
}

/* SDSTREAM: once a second, what came in that second. */
static void stats(int final)
{
    uint32_t now = ms(), dt = now - t_stats;
    const struct aa_hu_video *v = &hu.v;
    if (!AA_STREAM || !authed)
        return;
    if (final) {
        uint32_t secs = (now - t_video) / 1000;
        if (state != STREAMING && !v->messages && !hu.audio_messages)
            return;
        printf("summary: %lu video frames (%lu key frames) in %lu s, %lu KB, largest %lu bytes, %lu acknowledged; "
               "sound %lu KB in %lu messages; USB %lu KB; %lu ms decrypting\n",
            (unsigned long)v->messages, (unsigned long)v->idr, (unsigned long)secs, (unsigned long)(v->bytes / 1024),
            (unsigned long)v->largest, (unsigned long)v->acks, (unsigned long)(hu.audio_bytes / 1024),
            (unsigned long)hu.audio_messages, (unsigned long)(usb_bytes / 1024),
            (unsigned long)(decrypt_us / 1000));
        return;
    }
    if (dt < 1000)
        return;
    if (state == STREAMING || sound_seen || usb_bytes != last.usb)
        printf("%4lus: video %lu fps, %lu KB/s, %lu key; sound %lu KB/s; USB %lu KB/s, %lu frames; "
               "decrypting %lu%% of the time; largest %lu bytes, %lu acks, %lu pings, %lu lost\n",
            (unsigned long)((now - t_auth) / 1000), (unsigned long)((v->messages - last.messages) * 1000 / dt),
            (unsigned long)((v->bytes - last.video) * 1000 / 1024 / dt), (unsigned long)(v->idr - last.idr),
            (unsigned long)((hu.audio_bytes - last.audio) * 1000 / 1024 / dt),
            (unsigned long)((usb_bytes - last.usb) * 1000 / 1024 / dt), (unsigned long)(frames_in - last.frames),
            (unsigned long)((decrypt_us - last.decrypt) / 10 / dt), (unsigned long)v->largest,
            (unsigned long)v->acks, (unsigned long)hu.pings, (unsigned long)short_lost());
    last.usb = usb_bytes;
    last.video = v->bytes;
    last.audio = hu.audio_bytes;
    last.decrypt = decrypt_us;
    last.messages = v->messages;
    last.idr = v->idr;
    last.frames = frames_in;
    t_stats = now;
}

static void reset_counts(void)
{
    usb_bytes = decrypt_us = 0;
    frames_in = 0;
    authed = sound_seen = 0;
    t_auth = t_video = t_stats = ms();
    last.usb = last.video = last.decrypt = last.audio = 0;
    last.messages = last.idr = last.frames = 0;
}

static void core1(void)
{
    tuh_init(0);
    for (;;) {
        tuh_task();
        if (!aoa_device && state != WAITING && state != SWITCHING) {
            stats(1);
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
                reset_counts();
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
            const uint8_t *in = aoa_in;
            uint32_t at, k, len = aoa_in_len;
            if (aoa_in_done < 0) {
                fail("reading from the phone failed", 0);
                continue;
            }
            usb_bytes += len;
            /* The next transfer under way (into the other buffer) while
             * this one is decrypted and answered: the link does not wait
             * on the Pico.  Answers go out meanwhile (bulk OUT is not on
             * EPX: sdbridge/patches/). */
            if (aoa_read(daddr)) {
                fail("reading from the phone failed", 0);
                continue;
            }
            /* In pieces the reader has room for, the frames taken out
             * between them. */
            for (at = 0; at < len && state != FAILED; at += k) {
                k = len - at < AAP_FEED_MAX ? len - at : AAP_FEED_MAX;
                if (aap_feed(&reader, in + at, k)) {
                    fail("the phone's bytes make no frames", 0);
                    break;
                }
                frames();
            }
        }
        stats(0);
    }
}

static uint32_t core1_stack[16384 / 4];

int main(void)
{
    stdio_init_all();
    status_init(pio1, 0);
#if AA_STREAM
    printf("\nsdstream: waiting for a phone on the USB port (an 800x480 head unit with sound channels, "
           "%d frame%s ahead of the acknowledgements, video configuration %d fps, USB bulk %s)\n", AA_MAX_UNACKED,
           AA_MAX_UNACKED == 1 ? "" : "s", AA_HU_FPS == 2 ? 30 : 60,
           PICO_USB_HOST_BULK_DOUBLE_BUFFER ? "double-buffered" : "a packet at a time");
#else
    printf("\nsdhandshake: waiting for a phone on the USB port (with a self-signed test certificate, "
           "expect the phone to refuse it)\n");
#endif
    printf("certificate: %s\n", HU_CERT_WIRE ? "dates with a time zone; mbedTLS parses a copy in UTC, the phone gets it as it is"
                                             : "as given");
    multicore_launch_core1_with_stack(core1, core1_stack, sizeof core1_stack);
    for (;;) {
        status_tick();
        sleep_ms(5);
    }
}
