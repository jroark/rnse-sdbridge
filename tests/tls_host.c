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
 * completes, authentication complete goes across, and an encrypted
 * service discovery request comes back out.  Then one that trusts only
 * another authority, as a real phone trusts only Google's: it must end
 * the handshake with an alert, and the head unit must say which. */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../src/aa_tls.h"
#include "../src/aap.h"

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

static int run(const char *dir, const char *phone_trusts, int *alert)
{
    size_t cl, kl, pcl, pkl, cal, n = 0, m;
    char *cert = slurp(dir, "hu.crt", &cl), *key = slurp(dir, "hu.key", &kl);
    char *pcert = slurp(dir, "phone.crt", &pcl), *pkey = slurp(dir, "phone.key", &pkl);
    char *ca = slurp(dir, phone_trusts, &cal);
    int r_hu = 0, r_phone = 0, rounds;
    memset(&hu, 0, sizeof hu);
    memset(&phone, 0, sizeof phone);
    aap_reader_init(&hu.reader);
    aap_reader_init(&phone.reader);
    aap_assembler_init(&hu.msg);
    aap_assembler_init(&phone.msg);
    CHECK(aa_tls_init(&hu.tls, 0, cert, cl, key, kl, NULL, 0) == 0);
    CHECK(aa_tls_init(&phone.tls, 1, pcert, pcl, pkey, pkl, ca, cal) == 0);
    *alert = -1;
    /* The head unit starts. */
    r_hu = aa_tls_handshake(&hu.tls, NULL, 0, out, &n);
    for (rounds = 0; rounds < 20 && n; rounds++) {
        static uint8_t back[AA_TLS_BUF];
        r_phone = deliver_handshake(&phone, out, n, back, &m);
        if (!m)
            break;
        r_hu = deliver_handshake(&hu, back, m, out, &n);
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
    }
    aa_tls_free(&hu.tls);
    aa_tls_free(&phone.tls);
    r = run(argv[1], "other.crt", &alert);
    CHECK(r == MBEDTLS_ERR_SSL_FATAL_ALERT_MESSAGE);
    CHECK(alert == 48 || alert == 42 || alert == 46);
    printf("tls: a phone that trusts another authority refused the head unit with alert %d\n", alert);
    aa_tls_free(&hu.tls);
    aa_tls_free(&phone.tls);
    printf(failures ? "sdbridge tls tests: %d FAILED\n" : "sdbridge tls tests: all passed\n", failures);
    return failures != 0;
}
