/* TLS inside Android Auto's messages (src/aap.h): the handshake's bytes
 * travel as the payload of message 0x0003 on channel 0, and afterwards
 * each encrypted frame's payload is TLS records.  mbedTLS does the TLS;
 * this only moves its bytes between it and those payloads, through two
 * buffers in memory instead of a socket.
 *
 * The head unit is the client and presents a certificate of its own.
 * It does not check the phone's certificate (it has nothing to check it
 * against).  Plain C on mbedTLS, tested on the host against an mbedTLS
 * server standing in for the phone (tests/tls_host.c). */
#ifndef AA_TLS_H
#define AA_TLS_H
#include <stddef.h>
#include <stdint.h>
#include "mbedtls/ssl.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/pk.h"

/* A record of 16 KiB of plain text and what TLS adds to it (29 bytes
 * for AES-GCM, more for CBC with its MAC and padding): an encrypted
 * frame's payload can be that long. */
#define AA_TLS_BUF (16384 + 512)

struct aa_tls {
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_x509_crt crt, ca;
    mbedtls_pk_context key;
    /* The parsed certificate's own bytes, when crt.raw has been replaced
     * by the certificate as the phone is to see it (aa_tls_init). */
    unsigned char *parsed_raw;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_entropy_context entropy;
    /* Bytes from the other side not yet taken by mbedTLS, and bytes
     * mbedTLS has written that are still to be sent. */
    uint8_t in[AA_TLS_BUF];
    size_t in_len, in_pos;
    uint8_t out[AA_TLS_BUF];
    size_t out_len;
    int done;
};

/* The certificate and key in PEM, each with its terminating zero counted
 * in its length (as mbedTLS wants PEM).  wire, when not null, is the
 * certificate (DER) as it is to be sent, where cert is a copy that
 * mbedTLS can parse (sdbridge/pem2c.py: dates with a time zone).
 * server: the other side's role, for the host test; then ca (also PEM),
 * when not null, is the only authority whose client certificates it
 * accepts.  0, or mbedTLS's error. */
int aa_tls_init(struct aa_tls *t, int server, const char *cert, size_t cert_len, const uint8_t *wire, size_t wire_len,
                const char *key, size_t key_len, const char *ca, size_t ca_len);
void aa_tls_free(struct aa_tls *t);

/* A step of the handshake: the bytes of a handshake message from the
 * other side in (none for the client's first step), and in out (up to
 * AA_TLS_BUF) what to send back in one, *out_len of it (0: nothing).
 * 1 the handshake is done, 0 it goes on, else mbedTLS's error. */
int aa_tls_handshake(struct aa_tls *t, const uint8_t *in, size_t n, uint8_t *out, size_t *out_len);

/* An encrypted frame's payload: its records in, the plain text out
 * (*plain_len, up to cap).  0, or mbedTLS's error. */
int aa_tls_decrypt(struct aa_tls *t, const uint8_t *rec, size_t n, uint8_t *plain, size_t cap, size_t *plain_len);
/* Plain text into the records for an encrypted frame's payload. */
int aa_tls_encrypt(struct aa_tls *t, const uint8_t *plain, size_t n, uint8_t *rec, size_t cap, size_t *rec_len);

/* After MBEDTLS_ERR_SSL_FATAL_ALERT_MESSAGE: the alert's description
 * (TLS's numbers: 42 bad certificate, 48 unknown CA, ...). */
int aa_tls_alert(const struct aa_tls *t);
/* The cipher suite agreed, once done. */
const char *aa_tls_suite(const struct aa_tls *t);

#endif
