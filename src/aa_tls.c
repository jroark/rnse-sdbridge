#include <string.h>
#include "aa_tls.h"
#include "mbedtls/platform.h"

/* mbedTLS's socket, in memory. */
static int send_cb(void *ctx, const unsigned char *buf, size_t len)
{
    struct aa_tls *t = ctx;
    if (len > sizeof t->out - t->out_len)
        len = sizeof t->out - t->out_len;
    if (!len)
        return MBEDTLS_ERR_SSL_WANT_WRITE;
    memcpy(t->out + t->out_len, buf, len);
    t->out_len += len;
    return (int)len;
}

static int recv_cb(void *ctx, unsigned char *buf, size_t len)
{
    struct aa_tls *t = ctx;
    size_t have = t->in_len - t->in_pos;
    if (!have)
        return MBEDTLS_ERR_SSL_WANT_READ;
    if (len > have)
        len = have;
    memcpy(buf, t->in + t->in_pos, len);
    t->in_pos += len;
    return (int)len;
}

/* Bytes from the other side, after whatever is still unread. */
static int take(struct aa_tls *t, const uint8_t *p, size_t n)
{
    if (t->in_pos) {
        memmove(t->in, t->in + t->in_pos, t->in_len - t->in_pos);
        t->in_len -= t->in_pos;
        t->in_pos = 0;
    }
    if (n > sizeof t->in - t->in_len)
        return MBEDTLS_ERR_SSL_BUFFER_TOO_SMALL;
    memcpy(t->in + t->in_len, p, n);
    t->in_len += n;
    return 0;
}

/* What mbedTLS wrote, out to the caller. */
static int give(struct aa_tls *t, uint8_t *out, size_t cap, size_t *out_len)
{
    if (t->out_len > cap)
        return MBEDTLS_ERR_SSL_BUFFER_TOO_SMALL;
    memcpy(out, t->out, t->out_len);
    *out_len = t->out_len;
    t->out_len = 0;
    return 0;
}

int aa_tls_init(struct aa_tls *t, int server, const char *cert, size_t cert_len, const uint8_t *wire, size_t wire_len,
                const char *key, size_t key_len, const char *ca, size_t ca_len)
{
    static const char who[] = "rnse sdbridge";
    int r;
    memset(t, 0, sizeof *t);
    mbedtls_ssl_init(&t->ssl);
    mbedtls_ssl_config_init(&t->conf);
    mbedtls_x509_crt_init(&t->crt);
    mbedtls_x509_crt_init(&t->ca);
    mbedtls_pk_init(&t->key);
    mbedtls_ctr_drbg_init(&t->drbg);
    mbedtls_entropy_init(&t->entropy);
    if ((r = mbedtls_ctr_drbg_seed(&t->drbg, mbedtls_entropy_func, &t->entropy, (const unsigned char *)who, sizeof who)))
        return r;
    if ((r = mbedtls_x509_crt_parse(&t->crt, (const unsigned char *)cert, cert_len)))
        return r;
    if (wire) {
        /* mbedTLS sends crt.raw as the Certificate message.  The parsed
         * fields point into the old raw bytes, which are kept until
         * aa_tls_free; mbedtls_x509_crt_free frees the new ones (they are
         * the certificate's own buffer, own_buffer set by the parse). */
        unsigned char *p = mbedtls_calloc(1, wire_len);
        if (!p)
            return MBEDTLS_ERR_X509_ALLOC_FAILED;
        memcpy(p, wire, wire_len);
        t->parsed_raw = t->crt.raw.p;
        t->crt.raw.p = p;
        t->crt.raw.len = wire_len;
    }
    if ((r = mbedtls_pk_parse_key(&t->key, (const unsigned char *)key, key_len, NULL, 0)))
        return r;
    if ((r = mbedtls_ssl_config_defaults(&t->conf, server ? MBEDTLS_SSL_IS_SERVER : MBEDTLS_SSL_IS_CLIENT,
                                         MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT)))
        return r;
    mbedtls_ssl_conf_min_version(&t->conf, MBEDTLS_SSL_MAJOR_VERSION_3, MBEDTLS_SSL_MINOR_VERSION_3);
    mbedtls_ssl_conf_max_version(&t->conf, MBEDTLS_SSL_MAJOR_VERSION_3, MBEDTLS_SSL_MINOR_VERSION_3);
    mbedtls_ssl_conf_rng(&t->conf, mbedtls_ctr_drbg_random, &t->drbg);
    if ((r = mbedtls_ssl_conf_own_cert(&t->conf, &t->crt, &t->key)))
        return r;
    if (server && ca) {
        if ((r = mbedtls_x509_crt_parse(&t->ca, (const unsigned char *)ca, ca_len)))
            return r;
        mbedtls_ssl_conf_ca_chain(&t->conf, &t->ca, NULL);
        mbedtls_ssl_conf_authmode(&t->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    } else {
        mbedtls_ssl_conf_authmode(&t->conf, MBEDTLS_SSL_VERIFY_NONE);
    }
    if ((r = mbedtls_ssl_setup(&t->ssl, &t->conf)))
        return r;
    mbedtls_ssl_set_bio(&t->ssl, t, send_cb, recv_cb, NULL);
    return 0;
}

void aa_tls_free(struct aa_tls *t)
{
    mbedtls_ssl_free(&t->ssl);
    mbedtls_ssl_config_free(&t->conf);
    mbedtls_x509_crt_free(&t->crt);
    if (t->parsed_raw)
        mbedtls_free(t->parsed_raw);
    t->parsed_raw = NULL;
    mbedtls_x509_crt_free(&t->ca);
    mbedtls_pk_free(&t->key);
    mbedtls_ctr_drbg_free(&t->drbg);
    mbedtls_entropy_free(&t->entropy);
}

int aa_tls_handshake(struct aa_tls *t, const uint8_t *in, size_t n, uint8_t *out, size_t *out_len)
{
    int r;
    *out_len = 0;
    if (n && (r = take(t, in, n)))
        return r;
    r = t->done ? 0 : mbedtls_ssl_handshake(&t->ssl);
    if (r == 0)
        t->done = 1;
    else if (r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE) {
        /* An alert of our own may still be worth sending. */
        give(t, out, AA_TLS_OUT_BUF, out_len);
        return r;
    }
    if ((r = give(t, out, AA_TLS_OUT_BUF, out_len)))
        return r;
    return t->done;
}

int aa_tls_decrypt(struct aa_tls *t, const uint8_t *rec, size_t n, uint8_t *plain, size_t cap, size_t *plain_len)
{
    int r;
    *plain_len = 0;
    if ((r = take(t, rec, n)))
        return r;
    while (*plain_len < cap) {
        r = mbedtls_ssl_read(&t->ssl, plain + *plain_len, cap - *plain_len);
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == 0)
            break;
        if (r < 0)
            return r;
        *plain_len += (size_t)r;
    }
    return 0;
}

int aa_tls_encrypt(struct aa_tls *t, const uint8_t *plain, size_t n, uint8_t *rec, size_t cap, size_t *rec_len)
{
    size_t sent = 0;
    int r;
    while (sent < n) {
        r = mbedtls_ssl_write(&t->ssl, plain + sent, n - sent);
        if (r < 0)
            return r;
        sent += (size_t)r;
    }
    return give(t, rec, cap, rec_len);
}

int aa_tls_alert(const struct aa_tls *t)
{
    return t->ssl.in_msg ? t->ssl.in_msg[1] : -1;
}

const char *aa_tls_suite(const struct aa_tls *t)
{
    return mbedtls_ssl_get_ciphersuite(&t->ssl);
}
