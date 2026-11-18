/* Android Auto's frames, as a head unit sends and takes them over the
 * phone's accessory (AOA) bulk endpoints.  As far as aasdk (the open
 * source head unit library) has them; Google publishes no description.
 *
 *   byte 0     channel (0 = control)
 *   byte 1     flags: bits 1-0 the frame's place in a message (1 first,
 *              2 last, 3 both, 0 middle), bit 2 a control message on a
 *              channel other than 0, bit 3 encrypted
 *   bytes 2-3  the frame's payload length, most significant first
 *   (bytes 4-7 the whole message's length, on a first frame of several)
 *   payload    a message: its id (2 bytes) and its data
 *
 * Plain C with no SDK, tested on the host (sdbridge/tests/host.c). */
#ifndef AAP_H
#define AAP_H
#include <stdint.h>

#define AAP_VERSION_REQUEST 0x0001
#define AAP_VERSION_RESPONSE 0x0002
#define AAP_SSL_HANDSHAKE 0x0003
#define AAP_AUTH_COMPLETE 0x0004
#define AAP_SERVICE_DISCOVERY_REQUEST 0x0005
#define AAP_SERVICE_DISCOVERY_RESPONSE 0x0006
#define AAP_CHANNEL_OPEN_REQUEST 0x0007
#define AAP_CHANNEL_OPEN_RESPONSE 0x0008
#define AAP_PING_REQUEST 0x000b
#define AAP_PING_RESPONSE 0x000c
#define AAP_NAV_FOCUS_REQUEST 0x000d
#define AAP_NAV_FOCUS_RESPONSE 0x000e
#define AAP_SHUTDOWN_REQUEST 0x000f
#define AAP_SHUTDOWN_RESPONSE 0x0010
#define AAP_VOICE_SESSION_REQUEST 0x0011
#define AAP_AUDIO_FOCUS_REQUEST 0x0012
#define AAP_AUDIO_FOCUS_RESPONSE 0x0013

/* The largest frame payload.  The phone cuts a message into frames of
 * at most 16 KiB (aasdk's 0x4000), and encrypted that is a TLS record
 * of 16 KiB of plain text and what TLS adds to it (src/aa_tls.h). */
#define AAP_FRAME_MAX (16384 + 512)
/* The most bytes one aap_feed may take: the reader holds a frame's
 * worth that is not yet whole, and this much more. */
#define AAP_FEED_MAX 1024

#define AAP_FIRST 1
#define AAP_LAST 2
#define AAP_CONTROL 4
#define AAP_ENCRYPTED 8

/* The version request a head unit opens with (protocol 1.1, as aasdk
 * asks): its bytes into out, their number returned. */
int aap_version_request(uint8_t *out);

/* Frames out of a stream of bytes, which USB may cut anywhere. */
struct aap_reader {
    uint8_t buf[AAP_FRAME_MAX + 8 + AAP_FEED_MAX];
    uint32_t have;
    uint32_t consumed; /* the frame handed out last, still at the front */
};
struct aap_frame {
    uint8_t channel, flags;
    uint16_t length;
    const uint8_t *payload;
};
void aap_reader_init(struct aap_reader *r);
/* Bytes in (up to AAP_FEED_MAX, or more when the reader is empty); then
 * aap_next until it says 0.  -1: more than the buffer holds came
 * without a frame's end (the stream is out of step). */
int aap_feed(struct aap_reader *r, const uint8_t *p, uint32_t n);
/* The next whole frame, 1; 0 when there is none yet.  The frame's
 * payload stays good until the next aap_feed or aap_next. */
int aap_next(struct aap_reader *r, struct aap_frame *f);

/* One frame holding a whole message (first and last): its bytes into
 * out (n + 4 of them), their number returned. */
int aap_frame(uint8_t *out, uint8_t channel, uint8_t flags, const uint8_t *payload, uint16_t n);

/* A message out of its frames' payloads (decrypted first when they are
 * encrypted).  1 when a frame completes it (then buf and len hold it),
 * 0 when more is to come, -1 when frames come out of order or it is too
 * long. */
#ifndef AAP_MESSAGE_MAX
#define AAP_MESSAGE_MAX 32768
#endif
struct aap_assembler {
    uint8_t buf[AAP_MESSAGE_MAX];
    uint32_t len;
    int open;
};
void aap_assembler_init(struct aap_assembler *a);
int aap_assemble(struct aap_assembler *a, uint8_t flags, const uint8_t *p, uint32_t n);

/* "Authentication complete", status 0, as its message's payload. */
int aap_auth_complete(uint8_t *payload);

/* The fields of a protocol buffer, one at a time: from *at (start at
 * 0), the field's number and wire type, and for a varint its value, for
 * a length-delimited field its bytes.  1 a field, 0 the end, -1 not a
 * protocol buffer. */
struct aap_pb_field {
    uint32_t number, type;
    uint64_t value;
    const uint8_t *bytes;
    uint32_t len;
};
int aap_pb_next(const uint8_t *p, uint32_t n, uint32_t *at, struct aap_pb_field *f);

/* A protocol buffer written into a buffer of the caller's: fields one
 * after another, a message inside another written into a buffer of its
 * own first and added as bytes.  err is set (and nothing more written)
 * when the buffer is too small. */
struct aap_pb {
    uint8_t *p;
    uint32_t len, cap;
    int err;
};
void aap_pb_init(struct aap_pb *b, uint8_t *p, uint32_t cap);
void aap_pb_varint(struct aap_pb *b, uint32_t field, uint64_t v);
void aap_pb_bytes(struct aap_pb *b, uint32_t field, const void *p, uint32_t n);
void aap_pb_string(struct aap_pb *b, uint32_t field, const char *s);
/* A message's id (2 bytes) at the start of the buffer, for a message
 * whose fields follow. */
void aap_pb_id(struct aap_pb *b, uint16_t id);

/* A version response's major and minor version and its status (0 the
 * versions match); 0, or -1 when f is not one. */
int aap_version_response(const struct aap_frame *f, uint16_t *major, uint16_t *minor, uint16_t *status);

#endif
