/* Android Auto over TCP, from the host: the desk test SDSTREAM.UF2 is
 * (src/sdhandshake.c with AA_STREAM), with a socket where that has the
 * phone's USB endpoints.  It answers what a wireless bridge has to know
 * first: whether a phone will run a session over the network with no
 * Bluetooth before it, and at what rate.
 *
 *   aa_tcp connect PHONE [PORT] [SECONDS]
 *       to the phone's "head unit server" (Android Auto's developer
 *       settings, "Start head unit server"; port 5277), the phone and
 *       this machine on one network.
 *   aa_tcp listen [PORT] [SECONDS]
 *       waits for the phone to connect (port 5288), as a wireless head
 *       unit does once the phone has been told its address.
 *
 * Either way the head unit speaks first (the version request), is the
 * TLS client and presents the certificate the build was given
 * (tools/aa_tcp.sh: AA_CERT, AA_KEY, outside the repository).  The log
 * is SDSTREAM's: the handshake's steps, the service discovery request's
 * text, and once a second the video's and sound's rates.  Nothing is
 * kept.  SECONDS (default 30): how long to count after the video starts.
 *
 * Exit status: 0 video frames came, 1 the session did not get that far,
 * 2 no connection. */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include "aap.h"
#include "aa_tls.h"
#include "aa_hu.h"
#include "mbedtls/error.h"
#include "hu_cert.h"

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

enum { VERSION, HANDSHAKE, AUTHED, DISCOVERED, STREAMING, FAILED };
static int state = VERSION;
static int sock = -1;

static uint64_t net_bytes, decrypt_us;
static uint32_t frames_in;
static int sound_seen;
static uint32_t t_auth, t_video, t_stats;
static struct {
    uint64_t net, video, decrypt, audio;
    uint32_t messages, idr, frames;
} last;

int mbedtls_hardware_poll(void *data, unsigned char *out, size_t len, size_t *olen)
{
    int fd = open("/dev/urandom", O_RDONLY);
    ssize_t r;
    (void)data;
    if (fd < 0)
        return -1;
    r = read(fd, out, len);
    close(fd);
    *olen = r > 0 ? (size_t)r : 0;
    return r == (ssize_t)len ? 0 : -1;
}

static uint64_t us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;
}

static uint32_t ms(void)
{
    return (uint32_t)(us() / 1000u);
}

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
}

static int send_all(const uint8_t *p, size_t n)
{
    while (n) {
        ssize_t k = send(sock, p, n, 0);
        if (k < 0 && errno == EINTR)
            continue;
        if (k <= 0)
            return -1;
        p += k;
        n -= (size_t)k;
    }
    return 0;
}

/* What mbedTLS wrote, as a handshake message. */
static int send_handshake(const uint8_t *p, size_t n)
{
    size_t len = n + 2;
    if (!n)
        return 0;
    frame[0] = 0;
    frame[1] = AAP_FIRST | AAP_LAST;
    frame[2] = (uint8_t)(len >> 8);
    frame[3] = (uint8_t)len;
    frame[4] = 0x00;
    frame[5] = AAP_SSL_HANDSHAKE;
    memcpy(frame + 6, p, n);
    printf("handshake: %u bytes to the phone\n", (unsigned)n);
    return send_all(frame, len + 4);
}

static int send_encrypted(uint8_t channel, uint8_t flags, const uint8_t *p, uint32_t n)
{
    size_t rl;
    int r = aa_tls_encrypt(&tls, p, n, tls_out, sizeof tls_out, &rl);
    if (r) {
        fail("encrypting", r);
        return -1;
    }
    if (send_all(frame, (size_t)aap_frame(frame, channel, (uint8_t)(flags | AAP_ENCRYPTED), tls_out, (uint16_t)rl))) {
        fail("sending to the phone failed", 0);
        return -1;
    }
    return 0;
}

static void flush(void)
{
    unsigned i;
    for (i = 0; i < hu.nout && state != FAILED; i++)
        send_encrypted(hu.out[i].channel, hu.out[i].flags, hu.out[i].data, hu.out[i].len);
    hu.nout = 0;
}

static void handshake(const uint8_t *in, size_t n)
{
    size_t out_len;
    uint32_t t0 = ms();
    int r = aa_tls_handshake(&tls, in, n, tls_out, &out_len);
    printf("handshake: %u bytes from the phone, %lu ms here\n", (unsigned)n, (unsigned long)(ms() - t0));
    if (out_len && send_handshake(tls_out, out_len)) {
        fail("sending to the phone failed", 0);
        return;
    }
    if (r == MBEDTLS_ERR_SSL_FATAL_ALERT_MESSAGE) {
        int a = aa_tls_alert(&tls);
        printf("the phone ended the handshake with alert %d%s\n", a,
            a == 42 ? " (bad certificate)" : a == 48 ? " (unknown CA)" : a == 46 ? " (certificate unknown)" :
            a == 40 ? " (handshake failure)" : a == 43 ? " (unsupported certificate)" : "");
        state = FAILED;
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
        if (send_all(frame, (size_t)aap_frame(frame, 0, 0, payload, (uint16_t)k))) {
            fail("sending to the phone failed", 0);
            return;
        }
        printf("authentication complete sent\n");
        state = AUTHED;
        t_auth = t_stats = ms();
        last.net = net_bytes;
        last.decrypt = decrypt_us;
        last.frames = frames_in;
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
        }
        if (hu.v.config_new) {
            hu.v.config_new = 0;
            printf("H.264 profile %d (%s), constraints %02x, level %d.%d\n", hu.v.profile,
                hu.v.profile == 66 ? "Baseline" : hu.v.profile == 77 ? "Main" : hu.v.profile == 100 ? "High" : "?",
                hu.v.constraints & 0xff, hu.v.level / 10, hu.v.level % 10);
        }
    } else if (aap_assemble(&msg, f->flags, p, n) == 1) {
        if (state == AUTHED && f->channel == 0 && msg.len >= 2 && msg.buf[1] == AAP_SERVICE_DISCOVERY_REQUEST) {
            printf("the phone's service discovery request:\n");
            show_strings(msg.buf + 2, msg.len - 2, 0);
            state = DISCOVERED;
        }
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
            uint64_t t0 = us();
            int r = aa_tls_decrypt(&tls, f.payload, f.length, plain, sizeof plain, &n);
            decrypt_us += us() - t0;
            if (r) {
                fail("decrypting", r);
                return;
            }
            if (state >= AUTHED)
                session_frame(&f, plain, (uint32_t)n);
            continue;
        }
        if (aap_assemble(&msg, f.flags, f.payload, f.length) != 1)
            continue;
        if (state == HANDSHAKE && f.channel == 0 && msg.len >= 2 && msg.buf[0] == 0 && msg.buf[1] == AAP_SSL_HANDSHAKE)
            handshake(msg.buf + 2, msg.len - 2);
        else if (msg.len >= 2)
            printf("message %02x%02x on channel %d, %lu bytes, not encrypted\n", msg.buf[0], msg.buf[1], f.channel,
                (unsigned long)msg.len);
    }
}

static void stats(int final)
{
    uint32_t now = ms(), dt = now - t_stats;
    const struct aa_hu_video *v = &hu.v;
    if (state < AUTHED || (state == FAILED && !v->messages))
        return;
    if (final) {
        printf("summary: %lu video frames (%lu key frames) in %lu s, %lu KB, largest %lu bytes, %lu acknowledged; "
               "sound %lu KB in %lu messages; network %lu KB\n",
            (unsigned long)v->messages, (unsigned long)v->idr, (unsigned long)(v->messages ? (now - t_video) / 1000 : 0),
            (unsigned long)(v->bytes / 1024), (unsigned long)v->largest, (unsigned long)v->acks,
            (unsigned long)(hu.audio_bytes / 1024), (unsigned long)hu.audio_messages, (unsigned long)(net_bytes / 1024));
        return;
    }
    if (dt < 1000)
        return;
    if (state == STREAMING || sound_seen || net_bytes != last.net)
        printf("%4lus: video %lu fps, %lu KB/s, %lu key; sound %lu KB/s; network %lu KB/s, %lu frames; "
               "largest %lu bytes, %lu acks, %lu pings\n",
            (unsigned long)((now - t_auth) / 1000), (unsigned long)((v->messages - last.messages) * 1000 / dt),
            (unsigned long)((v->bytes - last.video) * 1000 / 1024 / dt), (unsigned long)(v->idr - last.idr),
            (unsigned long)((hu.audio_bytes - last.audio) * 1000 / 1024 / dt),
            (unsigned long)((net_bytes - last.net) * 1000 / 1024 / dt), (unsigned long)(frames_in - last.frames),
            (unsigned long)v->largest, (unsigned long)v->acks, (unsigned long)hu.pings);
    last.net = net_bytes;
    last.video = v->bytes;
    last.audio = hu.audio_bytes;
    last.decrypt = decrypt_us;
    last.messages = v->messages;
    last.idr = v->idr;
    last.frames = frames_in;
    t_stats = now;
    fflush(stdout);
}

static int open_connect(const char *host, const char *port)
{
    struct addrinfo hints, *res, *a;
    int s = -1, r;
    memset(&hints, 0, sizeof hints);
    hints.ai_socktype = SOCK_STREAM;
    if ((r = getaddrinfo(host, port, &hints, &res)) != 0) {
        printf("%s: %s\n", host, gai_strerror(r));
        return -1;
    }
    for (a = res; a; a = a->ai_next) {
        s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s < 0)
            continue;
        if (connect(s, a->ai_addr, a->ai_addrlen) == 0)
            break;
        printf("%s port %s: %s\n", host, port, strerror(errno));
        close(s);
        s = -1;
    }
    freeaddrinfo(res);
    return s;
}

static int open_listen(const char *port)
{
    struct sockaddr_in6 a, peer;
    socklen_t pl = sizeof peer;
    char text[INET6_ADDRSTRLEN];
    int one = 1, zero = 0, l = socket(AF_INET6, SOCK_STREAM, 0), s;
    if (l < 0)
        return -1;
    setsockopt(l, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    setsockopt(l, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof zero);
    memset(&a, 0, sizeof a);
    a.sin6_family = AF_INET6;
    a.sin6_port = htons((uint16_t)atoi(port));
    if (bind(l, (struct sockaddr *)&a, sizeof a) || listen(l, 1)) {
        printf("port %s: %s\n", port, strerror(errno));
        close(l);
        return -1;
    }
    printf("waiting for the phone on port %s\n", port);
    fflush(stdout);
    s = accept(l, (struct sockaddr *)&peer, &pl);
    close(l);
    if (s >= 0)
        printf("connection from %s\n", inet_ntop(AF_INET6, &peer.sin6_addr, text, sizeof text));
    return s;
}

int main(int argc, char **argv)
{
    static uint8_t in[65536];
    uint8_t m[16];
    int n, one = 1, listening;
    uint32_t seconds, t_open;
    const char *port;

    if (argc < 2 || (strcmp(argv[1], "connect") && strcmp(argv[1], "listen")) || (!strcmp(argv[1], "connect") && argc < 3)) {
        fprintf(stderr, "usage: aa_tcp connect PHONE [PORT] [SECONDS]\n       aa_tcp listen [PORT] [SECONDS]\n");
        return 2;
    }
    listening = !strcmp(argv[1], "listen");
    argv += listening ? 2 : 3;
    argc -= listening ? 2 : 3;
    port = argc > 0 ? argv[0] : listening ? "5288" : "5277";
    seconds = argc > 1 ? (uint32_t)atoi(argv[1]) : 30;

    sock = listening ? open_listen(port) : open_connect(argv[-1], port);
    if (sock < 0) {
        printf("no connection\n");
        return 2;
    }
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    printf("connected; certificate: %s\n", HU_CERT_WIRE ? "dates with a time zone, sent as it is" : "as given");

    aap_reader_init(&reader);
    aap_assembler_init(&msg);
    n = aap_version_request(m);
    if (send_all(m, (size_t)n)) {
        printf("sending the version request failed\n");
        return 1;
    }
    printf("version request sent\n");
    t_open = t_stats = ms();

    while (state != FAILED) {
        struct pollfd p = { sock, POLLIN, 0 };
        int r = poll(&p, 1, 200);
        if (r < 0 && errno != EINTR)
            break;
        if (r > 0) {
            ssize_t len = recv(sock, in, sizeof in, 0);
            size_t at, k;
            if (len == 0) {
                printf("the phone closed the connection\n");
                break;
            }
            if (len < 0) {
                printf("reading from the phone: %s\n", strerror(errno));
                break;
            }
            net_bytes += (uint64_t)len;
            for (at = 0; at < (size_t)len && state != FAILED; at += k) {
                k = (size_t)len - at < AAP_FEED_MAX ? (size_t)len - at : AAP_FEED_MAX;
                if (aap_feed(&reader, in + at, (uint32_t)k)) {
                    fail("the phone's bytes make no frames", 0);
                    break;
                }
                frames();
            }
        }
        stats(0);
        if (state == STREAMING && ms() - t_video >= seconds * 1000)
            break;
        if (state != STREAMING && ms() - t_open >= (seconds + 60) * 1000) {
            printf("no video after %lu s\n", (unsigned long)(seconds + 60));
            break;
        }
        fflush(stdout);
    }
    stats(1);
    close(sock);
    return hu.v.messages ? 0 : 1;
}
