/* src/aa_session.c on the host, against a stand-in phone: an mbedTLS
 * server behind Android Auto's frames, at the other end of two queues
 * of bytes in memory (the link, cut into pieces on the way as USB or
 * TCP cut it).  The session is driven only through what a firmware
 * calls: open, feed, step, flush, close.
 *
 * The version exchange, the handshake, "authentication complete", the
 * service discovery request answered, a video message in three frames
 * handed to the caller frame by frame and acknowledged (at once, and
 * with defer_video_acks only when the caller says), a key press sent,
 * a link that fails, and a second session after a close.
 *
 * tests/run.sh builds it with the certificates it makes for tls_host.c. */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../src/aa_session.h"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("%s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

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

static char *slurp(const char *dir, const char *name, size_t *len)
{
    char path[512];
    FILE *f;
    char *buf;
    long n;
    snprintf(path, sizeof path, "%s/%s", dir, name);
    f = fopen(path, "rb");
    if (!f) {
        perror(path);
        exit(2);
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc((size_t)n + 1);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n)
        exit(2);
    buf[n] = 0;
    fclose(f);
    *len = (size_t)n + 1;
    return buf;
}

/* ---- the link ------------------------------------------------------- */

#define QUEUE (8 * AA_TLS_BUF)
struct queue {
    uint8_t b[QUEUE];
    size_t len;
};
static struct queue to_phone, to_hu;
static int link_broken;

static void put(struct queue *q, const uint8_t *p, size_t n)
{
    if (n > sizeof q->b - q->len) {
        CHECK(!"queue full");
        return;
    }
    memcpy(q->b + q->len, p, n);
    q->len += n;
}

/* ---- the stand-in phone --------------------------------------------- */

static struct {
    struct aa_tls tls;
    struct aap_reader reader;
    struct aap_assembler msg;
    int versions, tls_done, authed;
    int may_refuse;       /* the test expects its handshake to fail */
    /* What the head unit sent, encrypted, by message id. */
    int discovery, acks, inputs, others;
} phone;
static uint8_t p_out[AA_TLS_BUF], p_wire[AA_TLS_BUF + 16], p_plain[AAP_FRAME_MAX];

/* A message from the phone in `pieces` frames, encrypted each. */
static void phone_sends(uint8_t channel, uint8_t flags, const uint8_t *m, uint32_t n, int pieces)
{
    uint32_t at = 0, piece = (n + (uint32_t)pieces - 1) / (uint32_t)pieces;
    while (at < n) {
        uint32_t k = n - at < piece ? n - at : piece, head = 4;
        size_t rl;
        uint8_t place = (uint8_t)((at == 0 ? AAP_FIRST : 0) | (at + k == n ? AAP_LAST : 0));
        CHECK(aa_tls_encrypt(&phone.tls, m + at, k, p_out, sizeof p_out, &rl) == 0);
        p_wire[0] = channel;
        p_wire[1] = (uint8_t)(flags | place | AAP_ENCRYPTED);
        p_wire[2] = (uint8_t)(rl >> 8);
        p_wire[3] = (uint8_t)rl;
        if (place == AAP_FIRST) {
            p_wire[4] = (uint8_t)(n >> 24);
            p_wire[5] = (uint8_t)(n >> 16);
            p_wire[6] = (uint8_t)(n >> 8);
            p_wire[7] = (uint8_t)n;
            head = 8;
        }
        memcpy(p_wire + head, p_out, rl);
        put(&to_hu, p_wire, head + rl);
        at += k;
    }
}

static void phone_message(uint8_t channel, int encrypted, const uint8_t *m, uint32_t n)
{
    uint16_t id;
    if (n < 2)
        return;
    id = (uint16_t)(m[0] << 8 | m[1]);
    if (!encrypted && channel == 0 && id == AAP_VERSION_REQUEST) {
        static const uint8_t answer[] = { 0x00, AAP_VERSION_RESPONSE, 0x00, 0x01, 0x00, 0x07, 0x00, 0x00 };
        phone.versions++;
        put(&to_hu, p_wire, (size_t)aap_frame(p_wire, 0, 0, answer, sizeof answer));
    } else if (!encrypted && channel == 0 && id == AAP_SSL_HANDSHAKE) {
        static uint8_t payload[AA_TLS_BUF + 2];
        size_t rl;
        int r = aa_tls_handshake(&phone.tls, m + 2, n - 2, payload + 2, &rl);
        CHECK(r >= 0 || phone.may_refuse);
        if (r == 1)
            phone.tls_done = 1;
        if (rl) {
            payload[0] = 0;
            payload[1] = AAP_SSL_HANDSHAKE;
            put(&to_hu, p_wire, (size_t)aap_frame(p_wire, 0, 0, payload, (uint16_t)(rl + 2)));
        }
    } else if (!encrypted && channel == 0 && id == AAP_AUTH_COMPLETE) {
        uint8_t req[64];
        struct aap_pb b;
        CHECK(phone.tls_done);
        phone.authed = 1;
        aap_pb_init(&b, req, sizeof req);
        aap_pb_id(&b, AAP_SERVICE_DISCOVERY_REQUEST);
        aap_pb_string(&b, 4, "stand-in phone");
        phone_sends(0, 0, req, b.len, 1);
    } else if (encrypted && channel == 0 && id == AAP_SERVICE_DISCOVERY_RESPONSE) {
        phone.discovery++;
    } else if (encrypted && channel == AA_CH_VIDEO && id == AA_AV_ACK) {
        phone.acks++;
    } else if (encrypted && channel == AA_CH_INPUT && id == AA_INPUT_EVENT) {
        phone.inputs++;
    } else {
        phone.others++;
    }
}

/* The phone takes what the head unit sent, in pieces of 64 bytes. */
static void phone_runs(void)
{
    size_t at = 0;
    while (at < to_phone.len) {
        struct aap_frame f;
        size_t k = to_phone.len - at < 64 ? to_phone.len - at : 64;
        CHECK(aap_feed(&phone.reader, to_phone.b + at, (uint32_t)k) == 0);
        at += k;
        while (aap_next(&phone.reader, &f)) {
            const uint8_t *p = f.payload;
            size_t n = f.length;
            if (f.flags & AAP_ENCRYPTED) {
                CHECK(phone.tls_done);
                if (aa_tls_decrypt(&phone.tls, f.payload, f.length, p_plain, sizeof p_plain, &n)) {
                    CHECK(!"the phone could not decrypt");
                    continue;
                }
                p = p_plain;
            }
            if (aap_assemble(&phone.msg, f.flags, p, (uint32_t)n) == 1)
                phone_message(f.channel, f.flags & AAP_ENCRYPTED, phone.msg.buf, phone.msg.len);
        }
    }
    to_phone.len = 0;
}

/* ---- the head unit's side, as a firmware has it ---------------------- */

static struct aa_session ses;
static uint64_t now_us;
static int media_frames, media_first, media_last, messages, states[8];
static uint32_t media_bytes;
static uint8_t media_sum;

static int io_send(void *ctx, const uint8_t *p, uint32_t n)
{
    (void)ctx;
    if (link_broken)
        return -1;
    put(&to_phone, p, n);
    return 0;
}

static uint64_t io_us(void *ctx)
{
    (void)ctx;
    return now_us += 1000;
}

static void io_media(void *ctx, uint8_t channel, uint8_t flags, const uint8_t *p, uint32_t n)
{
    uint32_t i;
    (void)ctx;
    CHECK(channel == AA_CH_VIDEO);
    media_frames++;
    if (flags & AAP_FIRST) {
        media_first++;
        CHECK(n >= 10 && p[0] == 0 && p[1] == AA_AV_MEDIA_TIMESTAMP);
    }
    if (flags & AAP_LAST)
        media_last++;
    media_bytes += n;
    for (i = 0; i < n; i++)
        media_sum = (uint8_t)(media_sum + p[i]);
}

static void io_message(void *ctx, uint8_t channel, const uint8_t *m, uint32_t n)
{
    (void)ctx, (void)channel, (void)m, (void)n;
    messages++;
}

static void io_state(void *ctx, int state)
{
    (void)ctx;
    if (state >= 0 && state < 8)
        states[state]++;
}

/* The link run until both sides have nothing more to say. */
static void run(void)
{
    int rounds;
    for (rounds = 0; rounds < 100 && (to_phone.len || to_hu.len); rounds++) {
        size_t at = 0, len;
        static uint8_t in[QUEUE];
        phone_runs();
        len = to_hu.len;
        memcpy(in, to_hu.b, len);
        to_hu.len = 0;
        while (at < len) {
            /* Uneven pieces, none longer than the reader takes. */
            size_t k = 1 + (at * 7 + 13) % AAP_FEED_MAX;
            if (k > len - at)
                k = len - at;
            if (aa_session_feed(&ses, in + at, (uint32_t)k))
                break;
            at += k;
            while (aa_session_step(&ses))
                ;
        }
    }
    CHECK(rounds < 100);
}

static void phone_starts(const char *dir, const char *trusts)
{
    size_t pcl, pkl, cal;
    char *pcert = slurp(dir, "phone.crt", &pcl), *pkey = slurp(dir, "phone.key", &pkl), *ca = slurp(dir, trusts, &cal);
    memset(&phone, 0, sizeof phone);
    aap_reader_init(&phone.reader);
    aap_assembler_init(&phone.msg);
    CHECK(aa_tls_init(&phone.tls, 1, pcert, pcl, NULL, 0, pkey, pkl, ca, cal) == 0);
    to_phone.len = to_hu.len = 0;
    free(pcert);
    free(pkey);
    free(ca);
}

/* A video message: the id, a timestamp, and n bytes of "picture". */
static uint8_t picture[40000];
static uint8_t video_message(uint32_t n)
{
    uint32_t i;
    uint8_t sum = 0;
    memset(picture, 0, 10);
    picture[1] = AA_AV_MEDIA_TIMESTAMP;
    for (i = 10; i < 10 + n; i++)
        picture[i] = (uint8_t)(i * 31 + 7);
    for (i = 0; i < 10 + n; i++)
        sum = (uint8_t)(sum + picture[i]);
    return sum;
}

int main(int argc, char **argv)
{
    size_t cl, kl;
    char *cert, *key;
    struct aa_session_io io = { NULL, io_send, io_us, io_media, io_message, io_state };
    struct aa_session_cert c;
    uint8_t sum;
    if (argc != 2) {
        fprintf(stderr, "usage: session_host DIR\n");
        return 2;
    }
    cert = slurp(argv[1], "hu.crt", &cl);
    key = slurp(argv[1], "hu.key", &kl);
    memset(&c, 0, sizeof c);
    c.cert = cert;
    c.cert_len = cl;
    c.key = key;
    c.key_len = kl;

    /* A session, acknowledgements at once. */
    aa_session_init(&ses, &io, &c, 1, 0);
    CHECK(ses.state == AA_S_IDLE);
    CHECK(aa_session_feed(&ses, (const uint8_t *)"x", 1) == -1);
    CHECK(aa_session_step(&ses) == 0);
    phone_starts(argv[1], "hu.crt");
    CHECK(aa_session_open(&ses) == 0);
    CHECK(ses.state == AA_S_VERSION);
    run();
    CHECK(phone.versions == 1 && phone.tls_done && phone.authed);
    CHECK(ses.state == AA_S_AUTHED);
    CHECK(states[AA_S_VERSION] == 1 && states[AA_S_HANDSHAKE] == 1 && states[AA_S_AUTHED] == 1 && !states[AA_S_FAILED]);
    CHECK(phone.discovery == 1);
    CHECK(messages == 1);
    CHECK(ses.frames_in > 3 && ses.bytes_in > 1000 && ses.decrypt_us > 0);

    sum = video_message(39000);
    phone_sends(AA_CH_VIDEO, 0, picture, 39010, 3);
    run();
    CHECK(media_frames == 3 && media_first == 1 && media_last == 1);
    CHECK(media_bytes == 39010 && media_sum == sum);
    CHECK(phone.acks == 1 && ses.hu.v.messages == 1);

    /* A key from the head unit. */
    aa_hu_key(&ses.hu, 1234, 0x17, 1);
    aa_session_flush(&ses);
    run();
    CHECK(phone.inputs == 1);
    CHECK(phone.others == 0);

    /* The link fails under an answer: the session fails, and stays so. */
    link_broken = 1;
    phone_sends(AA_CH_VIDEO, 0, picture, 1010, 1);
    run();
    CHECK(ses.state == AA_S_FAILED && states[AA_S_FAILED] == 1);
    CHECK(aa_session_step(&ses) == 0);
    CHECK(aa_session_feed(&ses, picture, 10) == -1);
    link_broken = 0;
    aa_session_close(&ses);
    CHECK(ses.state == AA_S_IDLE);
    aa_tls_free(&phone.tls);

    /* Again, the acknowledgements the caller's. */
    aa_session_init(&ses, &io, &c, 1, 1);
    phone_starts(argv[1], "hu.crt");
    media_frames = 0;
    CHECK(aa_session_open(&ses) == 0);
    run();
    CHECK(ses.state == AA_S_AUTHED && phone.discovery == 1);
    phone_sends(AA_CH_VIDEO, 0, picture, 5010, 2);
    run();
    CHECK(media_frames == 2 && phone.acks == 0);
    aa_hu_video_ack(&ses.hu);
    aa_session_flush(&ses);
    run();
    CHECK(phone.acks == 1);
    aa_session_close(&ses);
    aa_tls_free(&phone.tls);

    /* A phone that trusts another authority: the session fails in the
     * handshake. */
    memset(states, 0, sizeof states);
    phone_starts(argv[1], "other.crt");
    CHECK(aa_session_open(&ses) == 0);
    phone.may_refuse = 1;
    run();
    CHECK(ses.state == AA_S_FAILED && !states[AA_S_AUTHED]);
    aa_session_close(&ses);
    aa_tls_free(&phone.tls);

    free(cert);
    free(key);
    printf(failures ? "sdbridge session tests: %d FAILED\n" : "sdbridge session tests: all passed\n", failures);
    return failures != 0;
}
