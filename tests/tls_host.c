/* src/aa_tls.c on the host, as the head unit, against an mbedTLS server
 * standing in for the phone, every byte through Android Auto's frames
 * (src/aap.c) as it would go over USB:
 *
 *   sh sdbridge/tests/run.sh      (makes the test certificates, builds this)
 *
 *   tls_host DIR    DIR holding hu.crt/hu.key (the head unit, self-signed),
 *                   phone.crt/phone.key (the phone's), other.crt (an
 *                   authority that signed neither)
 *
 * First a "phone" that trusts the head unit's certificate: the handshake
 * completes, authentication complete goes across, and then the session
 * of src/aa_hu.c as the phone would run it: service discovery, the
 * channels opened, sensors, the video channel set up and started, and
 * video frames (one message over three frames) acknowledged.  Then one
 * that trusts only another authority, as a real phone trusts only
 * Google's: it must end the handshake with an alert, and the head unit
 * must say which.  Last, a head unit certificate whose dates have a time
 * zone (hu_tz.h, made by sdbridge/pem2c.py from DIR/hu-tz.crt): mbedTLS
 * refuses it as it is, takes pem2c.py's copy in UTC, and the bytes that
 * go to the phone are the certificate as it was. */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../src/aa_tls.h"
#include "../src/aap.h"
#include "../src/aa_hu.h"
#ifdef HU_TZ_HEADER
#include HU_TZ_HEADER
#endif

static int failures;
#define CHECK(c) do { if (!(c)) { printf("%s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

/* What the RP2040 port gets from its ring oscillator. */
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
    *len = (size_t)n + 1; /* PEM: with the zero */
    return buf;
}

/* One side's end of the USB: frames in, as a reader and a message. */
struct end {
    struct aa_tls tls;
    struct aap_reader reader;
    struct aap_assembler msg;
};

static struct end hu, phone;
static uint8_t wire[AA_TLS_BUF + 16], out[AA_TLS_BUF], plain[AAP_FRAME_MAX];

/* A handshake message from one side, through the frames, to the other:
 * the other's step.  Its result. */
static int deliver_handshake(struct end *to, const uint8_t *p, size_t n, uint8_t *reply, size_t *reply_len)
{
    static uint8_t payload[AA_TLS_BUF + 2];
    struct aap_frame f;
    int len, got = 0;
    size_t i;
    payload[0] = 0;
    payload[1] = AAP_SSL_HANDSHAKE;
    memcpy(payload + 2, p, n);
    len = aap_frame(wire, 0, 0, payload, (uint16_t)(n + 2));
    /* Cut in USB-sized pieces on the way. */
    for (i = 0; i < (size_t)len; i += 64)
        CHECK(aap_feed(&to->reader, wire + i, (size_t)len - i < 64 ? (size_t)len - i : 64) == 0);
    while (aap_next(&to->reader, &f))
        if (aap_assemble(&to->msg, f.flags, f.payload, f.length) == 1)
            got = 1;
    CHECK(got && to->msg.len == n + 2 && to->msg.buf[1] == AAP_SSL_HANDSHAKE);
    return aa_tls_handshake(&to->tls, to->msg.buf + 2, to->msg.len - 2, reply, reply_len);
}

/* Everything the head unit sent in the handshake. */
static uint8_t sent[4 * AA_TLS_BUF];
static size_t sent_len;

static void keep(const uint8_t *p, size_t n)
{
    if (n <= sizeof sent - sent_len) {
        memcpy(sent + sent_len, p, n);
        sent_len += n;
    }
}

/* A handshake with the head unit's certificate in DIR/hu.crt, or, with
 * hu_cert, that one (and wire, when not null, as aa_tls_init takes it). */
static int run_with(const char *dir, const char *phone_trusts, int *alert, const char *hu_cert, size_t hu_cert_len,
                    const uint8_t *wire, size_t wire_len)
{
    size_t cl = 0, kl, pcl, pkl, cal, n = 0, m;
    char *cert = hu_cert ? NULL : slurp(dir, "hu.crt", &cl), *key = slurp(dir, "hu.key", &kl);
    char *pcert = slurp(dir, "phone.crt", &pcl), *pkey = slurp(dir, "phone.key", &pkl);
    char *ca = slurp(dir, phone_trusts, &cal);
    int r_hu = 0, r_phone = 0, rounds;
    memset(&hu, 0, sizeof hu);
    memset(&phone, 0, sizeof phone);
    aap_reader_init(&hu.reader);
    aap_reader_init(&phone.reader);
    aap_assembler_init(&hu.msg);
    aap_assembler_init(&phone.msg);
    CHECK(aa_tls_init(&hu.tls, 0, hu_cert ? hu_cert : cert, hu_cert ? hu_cert_len : cl, wire, wire_len, key, kl, NULL, 0) == 0);
    CHECK(aa_tls_init(&phone.tls, 1, pcert, pcl, NULL, 0, pkey, pkl, ca, cal) == 0);
    *alert = -1;
    sent_len = 0;
    /* The head unit starts. */
    r_hu = aa_tls_handshake(&hu.tls, NULL, 0, out, &n);
    keep(out, n);
    for (rounds = 0; rounds < 20 && n; rounds++) {
        static uint8_t back[AA_TLS_BUF];
        r_phone = deliver_handshake(&phone, out, n, back, &m);
        if (!m)
            break;
        r_hu = deliver_handshake(&hu, back, m, out, &n);
        keep(out, n);
        if (r_hu < 0) {
            if (r_hu == MBEDTLS_ERR_SSL_FATAL_ALERT_MESSAGE)
                *alert = aa_tls_alert(&hu.tls);
            break;
        }
        if (r_hu == 1 && r_phone == 1 && !n)
            break;
    }
    /* The head unit's last flight may complete the phone. */
    if (r_hu == 1 && r_phone == 0 && n) {
        static uint8_t back[AA_TLS_BUF];
        r_phone = deliver_handshake(&phone, out, n, back, &m);
        if (m)
            r_hu = deliver_handshake(&hu, back, m, out, &n);
    }
    free(cert);
    free(key);
    free(pcert);
    free(pkey);
    free(ca);
    return r_hu == 1 && r_phone == 1 ? 1 : r_hu < 0 ? r_hu : -1;
}

static int run(const char *dir, const char *phone_trusts, int *alert)
{
    return run_with(dir, phone_trusts, alert, NULL, 0, NULL, 0);
}

static void after_handshake(void)
{
    struct aap_frame f;
    uint8_t payload[8], req[64];
    size_t n, rl;
    int len, k, got = 0;
    struct aap_pb_field field;
    uint32_t at = 0;
    /* Authentication complete, in the clear. */
    k = aap_auth_complete(payload);
    len = aap_frame(wire, 0, 0, payload, (uint16_t)k);
    CHECK(aap_feed(&phone.reader, wire, (uint32_t)len) == 0);
    CHECK(aap_next(&phone.reader, &f) == 1 && f.length == 4 && f.payload[1] == AAP_AUTH_COMPLETE && f.payload[2] == 8);
    /* The phone's service discovery request, encrypted: id 5, then field
     * 4 a name. */
    req[0] = 0;
    req[1] = AAP_SERVICE_DISCOVERY_REQUEST;
    req[2] = 0x22;
    req[3] = 10;
    memcpy(req + 4, "Test phone", 10);
    CHECK(aa_tls_encrypt(&phone.tls, req, 14, out, sizeof out, &rl) == 0);
    CHECK(rl > 14);
    len = aap_frame(wire, 0, AAP_ENCRYPTED, out, (uint16_t)rl);
    CHECK(aap_feed(&hu.reader, wire, (uint32_t)len) == 0);
    while (aap_next(&hu.reader, &f)) {
        CHECK(f.flags & AAP_ENCRYPTED);
        CHECK(aa_tls_decrypt(&hu.tls, f.payload, f.length, plain, sizeof plain, &n) == 0);
        if (aap_assemble(&hu.msg, f.flags, plain, (uint32_t)n) == 1)
            got = 1;
    }
    CHECK(got && hu.msg.len == 14 && hu.msg.buf[1] == AAP_SERVICE_DISCOVERY_REQUEST);
    CHECK(aap_pb_next(hu.msg.buf + 2, hu.msg.len - 2, &at, &field) == 1);
    CHECK(field.number == 4 && field.type == 2 && field.len == 10 && memcmp(field.bytes, "Test phone", 10) == 0);
    /* And the head unit's answer goes the other way. */
    CHECK(aa_tls_encrypt(&hu.tls, req, 14, out, sizeof out, &rl) == 0);
    CHECK(aa_tls_decrypt(&phone.tls, out, rl, plain, sizeof plain, &n) == 0 && n == 14 && memcmp(plain, req, 14) == 0);
}

/* The session after the handshake: the phone's messages, encrypted, in
 * frames, into the head unit as the firmware takes them
 * (src/sdhandshake.c, session_frame); its answers back out, decrypted
 * by the phone. */
static struct aa_hu session_hu;
struct answer {
    uint8_t channel, flags;
    uint16_t id;
    uint8_t body[AA_HU_MSG_MAX];
    uint32_t len;
};
static struct answer answers[16];
static int nanswers;

static void hu_takes(void)
{
    struct aap_frame f;
    size_t n;
    unsigned i;
    while (aap_next(&hu.reader, &f)) {
        CHECK(f.flags & AAP_ENCRYPTED);
        CHECK(aa_tls_decrypt(&hu.tls, f.payload, f.length, plain, sizeof plain, &n) == 0);
        if (!(aa_hu_media_channel(f.channel) && !(f.flags & AAP_CONTROL) &&
              aa_hu_media_frame(&session_hu, f.channel, f.flags, plain, (uint32_t)n))
            && aap_assemble(&hu.msg, f.flags, plain, (uint32_t)n) == 1)
            aa_hu_message(&session_hu, f.channel, f.flags, hu.msg.buf, hu.msg.len);
        /* The answers, encrypted by the head unit, to the phone. */
        for (i = 0; i < session_hu.nout; i++) {
            struct aa_hu_msg *m = &session_hu.out[i];
            struct aap_frame g;
            size_t rl, pl;
            int len;
            CHECK(aa_tls_encrypt(&hu.tls, m->data, m->len, out, sizeof out, &rl) == 0);
            len = aap_frame(wire, m->channel, (uint8_t)(m->flags | AAP_ENCRYPTED), out, (uint16_t)rl);
            CHECK(aap_feed(&phone.reader, wire, (uint32_t)len) == 0);
            while (aap_next(&phone.reader, &g)) {
                CHECK(aa_tls_decrypt(&phone.tls, g.payload, g.length, plain, sizeof plain, &pl) == 0);
                if (nanswers < 16 && pl >= 2) {
                    struct answer *a = &answers[nanswers++];
                    a->channel = g.channel;
                    a->flags = g.flags;
                    a->id = (uint16_t)(plain[0] << 8 | plain[1]);
                    a->len = (uint32_t)pl - 2;
                    memcpy(a->body, plain + 2, a->len);
                }
            }
        }
        session_hu.nout = 0;
    }
}

/* A message from the phone in `pieces` frames (encrypted each). */
static void phone_sends(uint8_t channel, uint8_t flags, const uint8_t *m, uint32_t n, int pieces)
{
    uint32_t at = 0, piece = (n + (uint32_t)pieces - 1) / (uint32_t)pieces;
    nanswers = 0;
    while (at < n) {
        uint32_t k = n - at < piece ? n - at : piece, head;
        size_t rl;
        uint8_t place = (uint8_t)((at == 0 ? AAP_FIRST : 0) | (at + k == n ? AAP_LAST : 0));
        uint32_t j;
        CHECK(aa_tls_encrypt(&phone.tls, m + at, k, out, sizeof out, &rl) == 0);
        wire[0] = channel;
        wire[1] = (uint8_t)(flags | place | AAP_ENCRYPTED);
        wire[2] = (uint8_t)(rl >> 8);
        wire[3] = (uint8_t)rl;
        head = 4;
        if (place == AAP_FIRST) {
            wire[4] = (uint8_t)(n >> 24);
            wire[5] = (uint8_t)(n >> 16);
            wire[6] = (uint8_t)(n >> 8);
            wire[7] = (uint8_t)n;
            head = 8;
        }
        memcpy(wire + head, out, rl);
        /* Fed as the firmware feeds the reader. */
        for (j = 0; j < head + rl; j += AAP_FEED_MAX) {
            uint32_t q = head + (uint32_t)rl - j < AAP_FEED_MAX ? head + (uint32_t)rl - j : AAP_FEED_MAX;
            CHECK(aap_feed(&hu.reader, wire + j, q) == 0);
            hu_takes();
        }
        at += k;
    }
}

static uint64_t answer_field(const struct answer *a, uint32_t number)
{
    struct aap_pb_field f;
    uint32_t at = 0;
    while (aap_pb_next(a->body, a->len, &at, &f) == 1)
        if (f.number == number && f.type == 0)
            return f.value;
    return 0xdead;
}

static void session(void)
{
    uint8_t m[64];
    static uint8_t video[40 * 1024];
    struct aap_pb b;
    struct aap_pb_field f, g;
    uint32_t at = 0, at2, i;
    int channels = 0, video_ok = 0, input_ok = 0, sensor_ok = 0, sound = 0, mic_ok = 0;
    aa_hu_init(&session_hu, 1);
    session_hu.quiet = 1;
    aap_reader_init(&hu.reader);
    aap_reader_init(&phone.reader);
    aap_assembler_init(&hu.msg);

    /* Service discovery. */
    aap_pb_init(&b, m, sizeof m);
    aap_pb_id(&b, AAP_SERVICE_DISCOVERY_REQUEST);
    aap_pb_string(&b, 4, "Test phone");
    aap_pb_string(&b, 5, "Test");
    phone_sends(0, 0, m, b.len, 1);
    CHECK(nanswers == 1 && answers[0].channel == 0 && answers[0].id == AAP_SERVICE_DISCOVERY_RESPONSE);
    while (aap_pb_next(answers[0].body, answers[0].len, &at, &f) == 1) {
        if (f.number == 1 && f.type == 2) {
            uint32_t id = 99;
            channels++;
            at2 = 0;
            while (aap_pb_next(f.bytes, f.len, &at2, &g) == 1) {
                if (g.number == 1)
                    id = (uint32_t)g.value;
                if (g.number == 3 && id == AA_CH_VIDEO) {
                    /* AV channel: stream type 3, a video config of 480p, 30 fps. */
                    struct aap_pb_field h2, c2;
                    uint32_t at3 = 0;
                    int type = 0, res = 0, fps = 0;
                    while (aap_pb_next(g.bytes, g.len, &at3, &h2) == 1) {
                        if (h2.number == 1)
                            type = (int)h2.value;
                        if (h2.number == 4) {
                            uint32_t at4 = 0;
                            while (aap_pb_next(h2.bytes, h2.len, &at4, &c2) == 1) {
                                if (c2.number == 1)
                                    res = (int)c2.value;
                                if (c2.number == 2)
                                    fps = (int)c2.value;
                            }
                        }
                    }
                    video_ok = type == 3 && res == 1 && fps == AA_HU_FPS;
                }
                if (g.number == 4 && id == AA_CH_INPUT)
                    input_ok = 1;
                if (g.number == 3 && id >= AA_CH_MEDIA_AUDIO && id <= AA_CH_SYSTEM_AUDIO) {
                    /* Sound: stream type 1, its audio type, a configuration. */
                    struct aap_pb_field h2, c2;
                    uint32_t at3 = 0, rate = 0, chans = 0;
                    int type = 0, atype = 0;
                    while (aap_pb_next(g.bytes, g.len, &at3, &h2) == 1) {
                        if (h2.number == 1)
                            type = (int)h2.value;
                        if (h2.number == 2)
                            atype = (int)h2.value;
                        if (h2.number == 3) {
                            uint32_t at4 = 0;
                            while (aap_pb_next(h2.bytes, h2.len, &at4, &c2) == 1) {
                                if (c2.number == 1)
                                    rate = (uint32_t)c2.value;
                                if (c2.number == 3)
                                    chans = (uint32_t)c2.value;
                            }
                        }
                    }
                    if (type == 1 && ((id == AA_CH_MEDIA_AUDIO && atype == 3 && rate == 48000 && chans == 2) ||
                                      (id != AA_CH_MEDIA_AUDIO && (atype == 1 || atype == 2) && rate == 16000 && chans == 1)))
                        sound++;
                }
                if (g.number == 5 && id == AA_CH_MIC)
                    mic_ok = 1;
                if (g.number == 2 && id == AA_CH_SENSOR)
                    sensor_ok = 1;
            }
        }
        if (f.number == 2)
            CHECK(f.len == 5 && memcmp(f.bytes, "RNS-E", 5) == 0);
    }
    CHECK(channels == 7 && video_ok && input_ok && sensor_ok && sound == 3 && mic_ok);

    /* The channels opened: on each, with the control flag. */
    for (i = 1; i <= 7; i++) {
        aap_pb_init(&b, m, sizeof m);
        aap_pb_id(&b, AAP_CHANNEL_OPEN_REQUEST);
        aap_pb_varint(&b, 1, 0);
        aap_pb_varint(&b, 2, i);
        phone_sends((uint8_t)i, AAP_CONTROL, m, b.len, 1);
        CHECK(nanswers == 1 && answers[0].channel == i && (answers[0].flags & AAP_CONTROL) &&
              answers[0].id == AAP_CHANNEL_OPEN_RESPONSE && answer_field(&answers[0], 1) == 0);
    }
    /* A channel the head unit does not have. */
    aap_pb_init(&b, m, sizeof m);
    aap_pb_id(&b, AAP_CHANNEL_OPEN_REQUEST);
    aap_pb_varint(&b, 2, 9);
    phone_sends(9, AAP_CONTROL, m, b.len, 1);
    CHECK(nanswers == 1 && answers[0].channel == 9 && answer_field(&answers[0], 1) == 1);

    /* Driving status: a start response and an event. */
    aap_pb_init(&b, m, sizeof m);
    aap_pb_id(&b, AA_SENSOR_START_REQUEST);
    aap_pb_varint(&b, 1, 13);
    phone_sends(AA_CH_SENSOR, 0, m, b.len, 1);
    CHECK(nanswers == 2 && answers[0].id == AA_SENSOR_START_RESPONSE && answer_field(&answers[0], 1) == 0);
    CHECK(answers[1].id == AA_SENSOR_EVENT && answers[1].len == 4 && answers[1].body[0] == (13 << 3 | 2));

    /* Input binding. */
    aap_pb_init(&b, m, sizeof m);
    aap_pb_id(&b, AA_INPUT_BINDING_REQUEST);
    aap_pb_varint(&b, 1, 3);
    phone_sends(AA_CH_INPUT, 0, m, b.len, 1);
    CHECK(nanswers == 1 && answers[0].id == AA_INPUT_BINDING_RESPONSE && answer_field(&answers[0], 1) == 0);

    /* Video: setup, then focus; start. */
    aap_pb_init(&b, m, sizeof m);
    aap_pb_id(&b, AA_AV_SETUP_REQUEST);
    aap_pb_varint(&b, 1, 0);
    phone_sends(AA_CH_VIDEO, 0, m, b.len, 1);
    CHECK(nanswers == 2 && answers[0].id == AA_AV_SETUP_RESPONSE && answer_field(&answers[0], 1) == 2 &&
          answer_field(&answers[0], 2) == 1);
    CHECK(answers[1].id == AA_VIDEO_FOCUS_INDICATION && answer_field(&answers[1], 1) == 1);
    aap_pb_init(&b, m, sizeof m);
    aap_pb_id(&b, AA_AV_START);
    aap_pb_varint(&b, 1, 7);
    aap_pb_varint(&b, 2, 0);
    phone_sends(AA_CH_VIDEO, 0, m, b.len, 1);
    CHECK(nanswers == 0 && session_hu.session == 7 && session_hu.video_started);

    /* The codec's configuration: a sequence parameter set, Baseline 3.1. */
    {
        static const uint8_t config[] = { 0x00, 0x01, 0, 0, 0, 1, 0x67, 66, 0xc0, 31, 0xda, 0, 0, 0, 1, 0x68, 0xce };
        phone_sends(AA_CH_VIDEO, 0, config, sizeof config, 1);
        CHECK(nanswers == 1 && answers[0].id == AA_AV_ACK && answer_field(&answers[0], 1) == 7 &&
              answer_field(&answers[0], 2) == 1);
        CHECK(session_hu.v.profile == 66 && session_hu.v.constraints == 0xc0 && session_hu.v.level == 31);
        CHECK(session_hu.v.configs == 1 && session_hu.v.config_new);
    }
    /* A 40 KiB key frame over three frames: one acknowledgement, after
     * the last. */
    video[0] = 0;
    video[1] = 0;                          /* with a timestamp */
    for (i = 2; i < 10; i++)
        video[i] = (uint8_t)i;
    video[10] = video[11] = video[12] = 0;
    video[13] = 1;
    video[14] = 0x65;                      /* an IDR slice */
    for (i = 15; i < sizeof video; i++)
        video[i] = (uint8_t)(i * 13 + 1);
    phone_sends(AA_CH_VIDEO, 0, video, sizeof video, 3);
    CHECK(nanswers == 1 && answers[0].id == AA_AV_ACK && answer_field(&answers[0], 1) == 7);
    CHECK(session_hu.v.messages == 2 && session_hu.v.idr == 1 && session_hu.v.largest == sizeof video);
    CHECK(session_hu.v.acks == 2 && session_hu.v.bytes == sizeof video + 17);
    /* Then an ordinary frame in one, not a key frame. */
    video[14] = 0x41;
    phone_sends(AA_CH_VIDEO, 0, video, 3000, 1);
    CHECK(nanswers == 1 && session_hu.v.messages == 3 && session_hu.v.idr == 1);
    /* As SDLINK.UF2 has it: no acknowledgement with the last frame, one
     * when the caller says (the 192 has read the picture). */
    session_hu.defer_video_acks = 1;
    phone_sends(AA_CH_VIDEO, 0, video, 3000, 2);
    CHECK(nanswers == 0 && session_hu.v.messages == 4 && session_hu.v.acks == 3);
    aa_hu_video_ack(&session_hu);
    CHECK(session_hu.nout == 1 && session_hu.v.acks == 4);
    session_hu.nout = 0;
    session_hu.defer_video_acks = 0;
    /* A key frame asked for: the focus away and back, in that order. */
    aa_hu_ask_key_frame(&session_hu);
    CHECK(session_hu.nout == 2 && session_hu.out[0].channel == AA_CH_VIDEO && session_hu.out[1].channel == AA_CH_VIDEO);
    session_hu.nout = 0;

    /* Sound on the media channel: set up, started, a message of PCM over
     * two frames, acknowledged with its own session. */
    aap_pb_init(&b, m, sizeof m);
    aap_pb_id(&b, AA_AV_SETUP_REQUEST);
    phone_sends(AA_CH_MEDIA_AUDIO, 0, m, b.len, 1);
    CHECK(nanswers == 1 && answers[0].channel == AA_CH_MEDIA_AUDIO && answers[0].id == AA_AV_SETUP_RESPONSE &&
          answer_field(&answers[0], 1) == 2);
    aap_pb_init(&b, m, sizeof m);
    aap_pb_id(&b, AA_AV_START);
    aap_pb_varint(&b, 1, 3);
    phone_sends(AA_CH_MEDIA_AUDIO, 0, m, b.len, 1);
    CHECK(nanswers == 0);
    video[14] = 0x41;
    phone_sends(AA_CH_MEDIA_AUDIO, 0, video, 8192, 2);
    CHECK(nanswers == 1 && answers[0].channel == AA_CH_MEDIA_AUDIO && answers[0].id == AA_AV_ACK &&
          answer_field(&answers[0], 1) == 3);
    CHECK(session_hu.audio_messages == 1 && session_hu.audio_bytes == 8192 && session_hu.v.messages == 4);
    /* The microphone opened: answered. */
    aap_pb_init(&b, m, sizeof m);
    aap_pb_id(&b, AA_AV_INPUT_OPEN_REQUEST);
    aap_pb_varint(&b, 1, 1);
    phone_sends(AA_CH_MIC, 0, m, b.len, 1);
    CHECK(nanswers == 1 && answers[0].channel == AA_CH_MIC && answers[0].id == AA_AV_INPUT_OPEN_RESPONSE);

    /* Video focus asked for again; a ping. */
    aap_pb_init(&b, m, sizeof m);
    aap_pb_id(&b, AA_VIDEO_FOCUS_REQUEST);
    aap_pb_varint(&b, 2, 1);
    phone_sends(AA_CH_VIDEO, 0, m, b.len, 1);
    CHECK(nanswers == 1 && answers[0].id == AA_VIDEO_FOCUS_INDICATION);
    aap_pb_init(&b, m, sizeof m);
    aap_pb_id(&b, AAP_PING_REQUEST);
    aap_pb_varint(&b, 1, 123456789012ull);
    phone_sends(0, 0, m, b.len, 1);
    CHECK(nanswers == 1 && answers[0].id == AAP_PING_RESPONSE && answer_field(&answers[0], 1) == 123456789012ull);
    /* Audio focus: gain asked, gain given. */
    aap_pb_init(&b, m, sizeof m);
    aap_pb_id(&b, AAP_AUDIO_FOCUS_REQUEST);
    aap_pb_varint(&b, 1, 1);
    phone_sends(0, 0, m, b.len, 1);
    CHECK(nanswers == 1 && answers[0].id == AAP_AUDIO_FOCUS_RESPONSE && answer_field(&answers[0], 1) == 1);
    printf("tls: the session: discovery, %d channels, sensors, video set up, %lu video frames acknowledged, "
           "sound, microphone\n", channels, (unsigned long)session_hu.v.acks);
}

/* A head unit certificate whose dates have a time zone. */
static void time_zone(const char *dir)
{
#ifdef HU_TZ_HEADER
    size_t tl, kl, i;
    char *tz = slurp(dir, "hu-tz.crt", &tl), *key = slurp(dir, "hu.key", &kl);
    int alert, found = 0;
    static struct aa_tls t;
    CHECK(HU_CERT_WIRE == 1);
    /* As it is, mbedTLS refuses it. */
    CHECK(aa_tls_init(&t, 0, tz, tl, NULL, 0, key, kl, NULL, 0) != 0);
    aa_tls_free(&t);
    /* The copy in UTC, with the certificate as it is to send. */
    CHECK(aa_tls_init(&t, 0, hu_cert_pem, sizeof hu_cert_pem, hu_cert_wire, sizeof hu_cert_wire, key, kl, NULL, 0) == 0);
    CHECK(t.crt.raw.len == sizeof hu_cert_wire && memcmp(t.crt.raw.p, hu_cert_wire, sizeof hu_cert_wire) == 0);
    aa_tls_free(&t);
    /* In a handshake (whatever the stand-in phone makes of the dates),
     * the Certificate message carries the bytes as they were. */
    run_with(dir, "hu.crt", &alert, hu_cert_pem, sizeof hu_cert_pem, hu_cert_wire, sizeof hu_cert_wire);
    for (i = 0; i + sizeof hu_cert_wire <= sent_len && !found; i++)
        found = memcmp(sent + i, hu_cert_wire, sizeof hu_cert_wire) == 0;
    CHECK(found);
    aa_tls_free(&hu.tls);
    aa_tls_free(&phone.tls);
    free(tz);
    free(key);
    printf("tls: a certificate with a time zone in its dates: parsed in UTC, sent as it is%s\n",
           alert >= 0 ? " (the stand-in phone refused its dates)" : "");
#else
    (void)dir;
    printf("tls: no hu_tz.h: the certificate with a time zone not tested\n");
#endif
}

int main(int argc, char **argv)
{
    int r, alert;
    if (argc != 2) {
        fprintf(stderr, "usage: tls_host DIR\n");
        return 2;
    }
    r = run(argv[1], "hu.crt", &alert);
    CHECK(r == 1);
    if (r == 1) {
        printf("tls: handshake done, %s\n", aa_tls_suite(&hu.tls));
        after_handshake();
        session();
    }
    aa_tls_free(&hu.tls);
    aa_tls_free(&phone.tls);
    r = run(argv[1], "other.crt", &alert);
    CHECK(r == MBEDTLS_ERR_SSL_FATAL_ALERT_MESSAGE);
    CHECK(alert == 48 || alert == 42 || alert == 46);
    printf("tls: a phone that trusts another authority refused the head unit with alert %d\n", alert);
    aa_tls_free(&hu.tls);
    aa_tls_free(&phone.tls);
    time_zone(argv[1]);
    printf(failures ? "sdbridge tls tests: %d FAILED\n" : "sdbridge tls tests: all passed\n", failures);
    return failures != 0;
}
