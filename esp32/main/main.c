/* The bridge on an ESP32-S3, all of it: the phone over Wi-Fi (its head
 * unit server over TCP, main/net.c) or on the chip's own USB port
 * (accessory mode, main/usb_aoa.c), the session the bridge's own
 * (src/aa_feed.c, as SDLINK.UF2 on the RP2040 has it), its video and
 * sound in the stream of AALINK.BIN (src/aastream.c), and the SD card
 * the unit reads that from (main/sdcard.c), which has the second core
 * to itself.  Everything else is on the first.
 *
 * Once a second the log has the video's rate, the link's, the time
 * spent decrypting, the stream and the card.  The log is on UART0, the
 * board's USB-serial port. */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "esp_attr.h"
#include "aa_feed.h"
#include "aa_streamsink.h"
#include "aastream.h"
#include "volume.h"
#include "sdcard.h"
#include "usb_aoa.h"
#include "net.h"
#include "hu_cert.h"

#ifndef AA_MAX_UNACKED
#define AA_MAX_UNACKED 1
#endif

static struct aa_feed *feed;
enum { LINK_NONE, LINK_USB, LINK_TCP };
static int via;
static int tcp = -1;
static uint8_t *tcp_buf;
#define TCP_BUF 16384

static uint64_t link_bytes;
static uint64_t tcp_heard;       /* when the phone last sent anything over TCP */
static unsigned tcp_departures;

/* ---- the stream, between the session (this core) and the card ------- */

#ifndef LINK_BLOCKS
#define LINK_BLOCKS 128
#endif
static struct aastream stream;
static struct aa_streamsink streamsink;
static uint32_t link_sector;          /* AALINK.BIN's first sector */
/* When the unit last started a read of the window (microseconds, the
 * low 32 bits). */
static volatile uint32_t last_window_read;

/* From the card's loop, on its core (sdcard.h). */
static void IRAM_ATTR link_words(uint32_t index, uint32_t *w)
{
    aastream_sector(&stream, index, w);
}

static void IRAM_ATTR card_read(uint32_t sector)
{
    uint32_t index;
    if (sector < link_sector)
        return;
    /* A read of AALINK.BIN's input or key sector: its event.  A read of
     * the window: the unit has what came before. */
    aastream_request(&stream, sector - link_sector);
    if (aastream_in_window(sector - link_sector, &index)) {
        aastream_consumed(&stream, index);
        last_window_read = (uint32_t)esp_timer_get_time();
    }
}

static void IRAM_ATTR card_sent(uint32_t sector)
{
    uint32_t index;
    if (sector >= link_sector && aastream_in_window(sector - link_sector, &index))
        aastream_went_out(&stream, aastream_seq(&stream, index));
}

static int reading(void *ctx)
{
    return (uint32_t)esp_timer_get_time() - last_window_read < 1000000u;
}

/* ---- the link -------------------------------------------------------- */

static int link_send(void *ctx, const uint8_t *p, uint32_t n)
{
    if (via == LINK_USB)
        return usb_aoa_send(p, n);
    if (via == LINK_TCP) {
        while (n) {
            int k = send(tcp, p, n, 0);
            if (k <= 0)
                return -1;
            p += k;
            n -= (uint32_t)k;
        }
        return 0;
    }
    return -1;
}

static uint64_t link_us(void *ctx)
{
    return (uint64_t)esp_timer_get_time();
}

static void link_down(const char *why)
{
    if (via == LINK_NONE)
        return;
    printf("%s\n", why);
    aa_feed_close(feed);
    if (tcp >= 0)
        close(tcp);
    tcp = -1;
    via = LINK_NONE;
}

static void on_event(void *ctx, int event)
{
    if (event == AA_S_FAILED)
        printf("the session failed\n");
}

/* ---- once a second --------------------------------------------------- */

static void stats(void)
{
    static uint64_t t_last, last_link, last_decrypt;
    static uint32_t last_videos;
    uint64_t now = link_us(NULL), dt = (now - t_last) / 1000;
    if (dt < 1000)
        return;
    if (feed->ses.state == AA_S_AUTHED && (feed->streaming || link_bytes != last_link))
        printf("%4lus: video %lu fps; %s %lu KB/s; decrypting %lu%% of the time; %lu acks; stream %lu of %u blocks, "
               "%lu pauses, sound dropped %lu; heap %lu\n",
            (unsigned long)((now - feed->ses.t_auth) / 1000000), (unsigned long)((feed->videos_in - last_videos) * 1000 / dt),
            via == LINK_TCP ? "TCP" : "USB", (unsigned long)((link_bytes - last_link) * 1000 / 1024 / dt),
            (unsigned long)((feed->ses.decrypt_us - last_decrypt) / 10 / dt), (unsigned long)feed->ses.hu.v.acks,
            (unsigned long)(stream.head - stream.tail), (unsigned)LINK_BLOCKS, (unsigned long)feed->pauses,
            (unsigned long)feed->sound_dropped, (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    sdcard_report(0);
    t_last = now;
    last_link = link_bytes;
    last_decrypt = feed->ses.decrypt_us;
    last_videos = feed->videos_in;
}

/* ---- the phone's task ------------------------------------------------ */

static void phone_task(void *arg)
{
    struct aa_session_cert cert = { hu_cert_pem, sizeof hu_cert_pem, HU_CERT_WIRE ? hu_cert_wire : NULL,
                                    HU_CERT_WIRE ? sizeof hu_cert_wire : 0, hu_key_pem, sizeof hu_key_pem };
    int wifi;
    feed = heap_caps_calloc(1, sizeof *feed, MALLOC_CAP_SPIRAM);
    tcp_buf = heap_caps_malloc(TCP_BUF, MALLOC_CAP_SPIRAM);
    if (!feed || !tcp_buf) {
        printf("no memory for the session (%u bytes): is the PSRAM there?\n", (unsigned)sizeof *feed);
        vTaskDelete(NULL);
    }
    {
        /* The stream's ring in the chip's own RAM, where the card's
         * loop reads it without the cache. */
        static uint8_t ring[LINK_BLOCKS][512] __attribute__((aligned(4)));
        struct aa_sink sink;
        aastream_init(&stream, ring, LINK_BLOCKS);
        aa_streamsink_init(&streamsink, &stream, reading, NULL);
        aa_streamsink_sink(&streamsink, &sink);
        feed->event = on_event;
        aa_feed_init(feed, link_send, link_us, NULL, &cert, AA_MAX_UNACKED, &sink);
    }
    link_sector = volume_link_sector();
    volume_link = link_words;
    usb_aoa_start();
    wifi = net_start();
    {
        /* The card last: its loop takes the second core for good. */
        static const struct sdcard_hooks hooks = { card_read, card_sent };
        sdcard_start(&hooks);
    }
    printf("s3bridge: an SD card with AALINK.BIN (the phone's video, %u blocks of RAM) on GPIO %d/%d/%d; waiting for a phone "
           "on the USB port%s\n", (unsigned)LINK_BLOCKS, SDCARD_PIN_CMD, SDCARD_PIN_CLK, SDCARD_PIN_DAT0,
        wifi ? ", or its head unit server on the network" : "");
    for (;;) {
        int busy = via != LINK_NONE && (!aa_feed_idle(feed) || (via == LINK_USB && usb_aoa_in_done));
        usb_aoa_poll(busy ? 0 : 1);

        /* USB: an accessory just opened takes the link (from TCP too). */
        if (usb_aoa_fresh) {
            usb_aoa_fresh = 0;
            link_down("a phone on USB: the other link is dropped");
            via = LINK_USB;
            link_bytes = 0;
            if (aa_feed_open(feed) || usb_aoa_read())
                link_down("the accessory's endpoints did not work");
        }
        if (via == LINK_USB && !usb_aoa_up())
            link_down("the phone is gone");
        if (via == LINK_USB && feed->ses.state != AA_S_FAILED && aa_feed_idle(feed) && usb_aoa_in_done) {
            if (usb_aoa_in_done < 0) {
                link_down("reading from the phone failed");
            } else {
                link_bytes += usb_aoa_in_len;
                aa_feed_give(feed, usb_aoa_in, usb_aoa_in_len);
                if (usb_aoa_read())
                    link_down("reading from the phone failed");
            }
        }

        /* TCP, while there is no phone on USB. */
        net_want(wifi && via == LINK_NONE);
        if (wifi && via == LINK_NONE && (tcp = net_take()) >= 0) {
            int flags = fcntl(tcp, F_GETFL, 0);
            fcntl(tcp, F_SETFL, flags | O_NONBLOCK);
            via = LINK_TCP;
            link_bytes = 0;
            tcp_heard = link_us(NULL);
            tcp_departures = net_departures();
            if (aa_feed_open(feed))
                link_down("the connection did not work");
        } else if (wifi && via != LINK_NONE) {
            int s = net_take();
            if (s >= 0)
                close(s);
        }
        if (via == LINK_TCP && feed->ses.state == AA_S_FAILED) {
            link_down("the session over TCP failed: the connection is closed");
        } else if (via == LINK_TCP && aa_feed_idle(feed)) {
            int n = recv(tcp, tcp_buf, TCP_BUF, 0);
            if (n > 0) {
                tcp_heard = link_us(NULL);
                link_bytes += (uint32_t)n;
                aa_feed_give(feed, tcp_buf, (uint32_t)n);
            } else if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
                link_down("the phone closed the connection");
            }
        }

        /* A phone that left the network, or went out of reach, closes
         * nothing: its connection is given up when a device has left or
         * nothing has come for six seconds (the phone sends a ping every
         * second or so), and the next is tried. */
        if (via == LINK_TCP && net_departures() != tcp_departures)
            link_down("a device left the network: the connection is given up");
        else if (via == LINK_TCP && link_us(NULL) - tcp_heard > 6000000)
            link_down("nothing from the phone for 6 s: the connection is given up");

        if (via != LINK_NONE)
            aa_feed_run(feed);
        stats();
    }
}

void app_main(void)
{
    /* The handshake's key operations want a deep stack. */
    xTaskCreatePinnedToCore(phone_task, "phone", 16384, NULL, 5, NULL, 0);
}
