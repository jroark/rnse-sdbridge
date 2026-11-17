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

#define AAP_FIRST 1
#define AAP_LAST 2
#define AAP_CONTROL 4
#define AAP_ENCRYPTED 8

/* The version request a head unit opens with (protocol 1.1, as aasdk
 * asks): its bytes into out, their number returned. */
int aap_version_request(uint8_t *out);

/* Frames out of a stream of bytes, which USB may cut anywhere. */
struct aap_reader {
    uint8_t buf[2048];
    uint32_t have;
    uint32_t consumed; /* the frame handed out last, still at the front */
};
struct aap_frame {
    uint8_t channel, flags;
    uint16_t length;
    const uint8_t *payload;
};
void aap_reader_init(struct aap_reader *r);
/* Bytes in; then aap_next until it says 0.  -1: more than the buffer
 * holds came without a frame's end (the stream is out of step). */
int aap_feed(struct aap_reader *r, const uint8_t *p, uint32_t n);
/* The next whole frame, 1; 0 when there is none yet.  The frame's
 * payload stays good until the next aap_feed or aap_next. */
int aap_next(struct aap_reader *r, struct aap_frame *f);

/* A version response's major and minor version and its status (0 the
 * versions match); 0, or -1 when f is not one. */
int aap_version_response(const struct aap_frame *f, uint16_t *major, uint16_t *minor, uint16_t *status);

#endif
