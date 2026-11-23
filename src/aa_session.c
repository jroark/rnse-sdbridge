#include <stdio.h>
#include "aa_session.h"
#include "mbedtls/error.h"

static void set_state(struct aa_session *s, int state)
{
    s->state = state;
    if (s->io.state)
        s->io.state(s->io.ctx, state);
}

static void fail(struct aa_session *s, const char *why, int r)
{
    if (r) {
        char text[100];
        mbedtls_strerror(r, text, sizeof text);
        printf("%s: %s (-0x%04x)\n", why, text, (unsigned)-r);
    } else {
        printf("%s\n", why);
    }
    set_state(s, AA_S_FAILED);
}

static int out(struct aa_session *s, const uint8_t *p, uint32_t n)
{
    if (s->io.send(s->io.ctx, p, n)) {
        fail(s, "sending to the phone failed", 0);
        return -1;
    }
    return 0;
}

/* What mbedTLS wrote, as a handshake message. */
static int send_handshake(struct aa_session *s, const uint8_t *p, size_t n)
{
    size_t i, len = n + 2;
    uint8_t *frame = s->frame;
    if (!n)
        return 0;
    if (len + 4 > sizeof s->frame) {
        fail(s, "a handshake message too long to send", 0);
        return -1;
    }
    frame[0] = 0;
    frame[1] = AAP_FIRST | AAP_LAST;
    frame[2] = (uint8_t)(len >> 8);
    frame[3] = (uint8_t)len;
    frame[4] = 0x00;
    frame[5] = AAP_SSL_HANDSHAKE;
    for (i = 0; i < n; i++)
        frame[6 + i] = p[i];
    return out(s, frame, (uint32_t)(len + 4));
}

/* A message, encrypted, as one frame. */
static int send_encrypted(struct aa_session *s, uint8_t channel, uint8_t flags, const uint8_t *p, uint32_t n)
{
    size_t rl;
    int r = aa_tls_encrypt(&s->tls, p, n, s->tls_out, sizeof s->tls_out, &rl);
    if (r) {
        fail(s, "encrypting", r);
        return -1;
    }
    return out(s, s->frame, (uint32_t)aap_frame(s->frame, channel, (uint8_t)(flags | AAP_ENCRYPTED), s->tls_out, (uint16_t)rl));
}

void aa_session_flush(struct aa_session *s)
{
    unsigned i;
    if (s->state != AA_S_AUTHED) {
        s->hu.nout = 0;
        return;
    }
    for (i = 0; i < s->hu.nout && s->state == AA_S_AUTHED; i++)
        send_encrypted(s, s->hu.out[i].channel, s->hu.out[i].flags, s->hu.out[i].data, s->hu.out[i].len);
    s->hu.nout = 0;
}

/* A step of the handshake, with the bytes the phone sent (or none). */
static void handshake(struct aa_session *s, const uint8_t *in, size_t n)
{
    size_t out_len;
    uint64_t t0 = s->io.us(s->io.ctx);
    int r = aa_tls_handshake(&s->tls, in, n, s->tls_out, &out_len);
    printf("handshake: %u bytes from the phone, %lu ms\n", (unsigned)n,
        (unsigned long)((s->io.us(s->io.ctx) - t0) / 1000));
    if (out_len && send_handshake(s, s->tls_out, out_len))
        return;
    if (r == MBEDTLS_ERR_SSL_FATAL_ALERT_MESSAGE) {
        int a = aa_tls_alert(&s->tls);
        printf("the phone ended the handshake with alert %d%s\n", a,
            a == 42 ? " (bad certificate)" : a == 48 ? " (unknown CA)" : a == 46 ? " (certificate unknown)" :
            a == 40 ? " (handshake failure)" : a == 43 ? " (unsupported certificate)" : "");
        set_state(s, AA_S_FAILED);
        return;
    }
    if (r < 0) {
        fail(s, "handshake", r);
        return;
    }
    if (r == 1) {
        uint8_t payload[8];
        int k = aap_auth_complete(payload);
        printf("handshake done: %s\n", aa_tls_suite(&s->tls));
        if (out(s, s->frame, (uint32_t)aap_frame(s->frame, 0, 0, payload, (uint16_t)k)))
            return;
        aa_hu_init(&s->hu, s->max_unacked);
        s->hu.defer_video_acks = s->defer_video_acks;
        s->t_auth = s->io.us(s->io.ctx);
        set_state(s, AA_S_AUTHED);
    }
}

/* A decrypted frame, the session running. */
static void session_frame(struct aa_session *s, const struct aap_frame *f, const uint8_t *p, uint32_t n)
{
    if (aa_hu_media_channel(f->channel) && !(f->flags & AAP_CONTROL) &&
        aa_hu_media_frame(&s->hu, f->channel, f->flags, p, n)) {
        if (s->io.media)
            s->io.media(s->io.ctx, f->channel, f->flags, p, n);
    } else if (aap_assemble(&s->msg, f->flags, p, n) == 1) {
        if (s->io.message)
            s->io.message(s->io.ctx, f->channel, s->msg.buf, s->msg.len);
        aa_hu_message(&s->hu, f->channel, f->flags, s->msg.buf, s->msg.len);
    }
    aa_session_flush(s);
}

static void one_frame(struct aa_session *s, const struct aap_frame *f)
{
    uint16_t major, minor, st;
    if (s->state == AA_S_VERSION) {
        if (aap_version_response(f, &major, &minor, &st) == 0) {
            printf("Android Auto answered: version %u.%u, %s\n", major, minor, st == 0 ? "they match" : "they do not match");
            if (st != 0) {
                fail(s, "version mismatch", 0);
                return;
            }
            set_state(s, AA_S_HANDSHAKE);
            if (aa_tls_init(&s->tls, 0, s->cert.cert, s->cert.cert_len, s->cert.wire, s->cert.wire_len, s->cert.key,
                            s->cert.key_len, NULL, 0)) {
                fail(s, "the certificate or key the build was given did not parse", 0);
                return;
            }
            s->tls_up = 1;
            handshake(s, NULL, 0);
        }
        return;
    }
    if (f->flags & AAP_ENCRYPTED) {
        size_t n;
        uint64_t t0;
        int r;
        if (s->state != AA_S_AUTHED)
            return;
        t0 = s->io.us(s->io.ctx);
        r = aa_tls_decrypt(&s->tls, f->payload, f->length, s->plain, sizeof s->plain, &n);
        s->decrypt_us += s->io.us(s->io.ctx) - t0;
        if (r) {
            fail(s, "decrypting", r);
            return;
        }
        session_frame(s, f, s->plain, (uint32_t)n);
        return;
    }
    if (aap_assemble(&s->msg, f->flags, f->payload, f->length) != 1)
        return;
    if (s->state == AA_S_HANDSHAKE && f->channel == 0 && s->msg.len >= 2 && s->msg.buf[0] == 0 &&
        s->msg.buf[1] == AAP_SSL_HANDSHAKE)
        handshake(s, s->msg.buf + 2, s->msg.len - 2);
}

void aa_session_init(struct aa_session *s, const struct aa_session_io *io, const struct aa_session_cert *cert,
                     int max_unacked, int defer_video_acks)
{
    s->state = AA_S_IDLE;
    s->io = *io;
    s->cert = *cert;
    s->max_unacked = max_unacked;
    s->defer_video_acks = defer_video_acks;
    s->tls_up = 0;
    s->hu.nout = 0;
}

int aa_session_open(struct aa_session *s)
{
    uint8_t m[16];
    int n = aap_version_request(m);
    aa_session_close(s);
    aap_reader_init(&s->reader);
    aap_assembler_init(&s->msg);
    s->bytes_in = s->decrypt_us = 0;
    s->frames_in = 0;
    if (s->io.send(s->io.ctx, m, (uint32_t)n)) {
        fail(s, "sending the version request failed", 0);
        return -1;
    }
    set_state(s, AA_S_VERSION);
    return 0;
}

void aa_session_close(struct aa_session *s)
{
    if (s->tls_up) {
        aa_tls_free(&s->tls);
        s->tls_up = 0;
    }
    s->hu.nout = 0;
    if (s->state != AA_S_IDLE)
        set_state(s, AA_S_IDLE);
}

int aa_session_feed(struct aa_session *s, const uint8_t *p, uint32_t n)
{
    if (s->state == AA_S_IDLE || s->state == AA_S_FAILED)
        return -1;
    s->bytes_in += n;
    if (aap_feed(&s->reader, p, n)) {
        fail(s, "the phone's bytes make no frames", 0);
        return -1;
    }
    return 0;
}

int aa_session_step(struct aa_session *s)
{
    struct aap_frame f;
    if (s->state == AA_S_IDLE || s->state == AA_S_FAILED)
        return 0;
    if (!aap_next(&s->reader, &f))
        return 0;
    s->frames_in++;
    one_frame(s, &f);
    return 1;
}
