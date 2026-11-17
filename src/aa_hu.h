/* The head unit's side of an Android Auto session once the TLS
 * handshake is done and "authentication complete" sent: the answer to
 * the phone's service discovery request, its channels opened, and the
 * video channel set up, focused and its frames acknowledged.  What the
 * phone sends is counted, not kept: this is for measuring what comes
 * over the link (src/sdhandshake.c built as SDSTREAM.UF2).
 *
 * The head unit offers the channels as aasdk numbers them: input (1, an
 * 800x480 touch screen), sensors (2, driving status and night mode),
 * video (3, 800x480 at 30 frames a second), sound (4 media, 48 kHz
 * stereo; 5 speech and 6 system, 16 kHz mono) and a microphone (7, 16
 * kHz mono).  A phone of 2026 will not start a session without the
 * sound channels (its log: "startRequiredServices failed").  Sound is
 * acknowledged and counted like video; nothing is ever sent from the
 * microphone.  The message ids and the protocol buffers' fields are as
 * aasdk has them (Google publishes no description).
 *
 * Plain C with no SDK, tested on the host (sdbridge/tests/tls_host.c). */
#ifndef AA_HU_H
#define AA_HU_H
#include <stdint.h>

#define AA_CH_CONTROL 0
#define AA_CH_INPUT 1
#define AA_CH_SENSOR 2
#define AA_CH_VIDEO 3
#define AA_CH_MEDIA_AUDIO 4
#define AA_CH_SPEECH_AUDIO 5
#define AA_CH_SYSTEM_AUDIO 6
#define AA_CH_MIC 7
#define AA_CHANNELS 8

/* Video (and sound) channel messages. */
#define AA_AV_MEDIA_TIMESTAMP 0x0000 /* an 8-byte timestamp, then H.264 */
#define AA_AV_MEDIA 0x0001           /* H.264 with no timestamp: the codec's configuration */
#define AA_AV_SETUP_REQUEST 0x8000
#define AA_AV_START 0x8001
#define AA_AV_STOP 0x8002
#define AA_AV_SETUP_RESPONSE 0x8003
#define AA_AV_ACK 0x8004
#define AA_AV_INPUT_OPEN_REQUEST 0x8005 /* the microphone */
#define AA_AV_INPUT_OPEN_RESPONSE 0x8006
#define AA_VIDEO_FOCUS_REQUEST 0x8007
#define AA_VIDEO_FOCUS_INDICATION 0x8008
/* Sensor channel. */
#define AA_SENSOR_START_REQUEST 0x8001
#define AA_SENSOR_START_RESPONSE 0x8002
#define AA_SENSOR_EVENT 0x8003
/* Input channel. */
#define AA_INPUT_EVENT 0x8001
#define AA_INPUT_BINDING_REQUEST 0x8002
#define AA_INPUT_BINDING_RESPONSE 0x8003

#define AA_HU_WIDTH 800
#define AA_HU_HEIGHT 480

/* A message for the phone: to be encrypted and sent as one frame on
 * `channel` with `flags` (AAP_CONTROL or 0). */
#define AA_HU_MSG_MAX 512
struct aa_hu_msg {
    uint8_t channel, flags;
    uint16_t len;
    uint8_t data[AA_HU_MSG_MAX];
};

struct aa_hu_video {
    uint32_t messages;     /* media messages taken whole */
    uint32_t idr;          /* of them, key frames (an IDR slice) */
    uint64_t bytes;        /* media bytes, ids and timestamps included */
    uint32_t largest;      /* the longest media message */
    uint32_t acks;
    uint32_t configs;      /* codec configuration messages */
    int profile, constraints, level; /* from the sequence parameter set, -1 before one */
    uint8_t config[48];    /* the start of the last codec configuration */
    uint32_t config_len;
    int config_new;        /* set by one; the caller clears it */
};

#define AA_HU_QUEUE 8
struct aa_hu {
    int max_unacked;
    int quiet;             /* no log */
    int session;           /* the video session, from its start indication */
    int video_started;
    uint32_t pings;
    struct aa_hu_video v;
    /* Each channel's media message whose frames are coming, and its
     * session. */
    struct {
        int in_media;
        uint32_t msg_bytes;
        int session;
    } ch[AA_CHANNELS];
    /* Sound, all channels together. */
    uint64_t audio_bytes;
    uint32_t audio_messages;
    unsigned nout;
    struct aa_hu_msg out[AA_HU_QUEUE];
};

/* max_unacked: the video frames the phone may send ahead of the
 * acknowledgements (aasdk's head unit says 1). */
void aa_hu_init(struct aa_hu *h, int max_unacked);

/* A whole message from the phone, decrypted, id first: on `channel`
 * with its frames' `flags`.  Answers go into out (nout of them), for
 * the caller to send in order and then set nout to 0.  1 when the
 * message was known, 0 when it was not (it is logged). */
int aa_hu_message(struct aa_hu *h, uint8_t channel, uint8_t flags, const uint8_t *m, uint32_t n);

/* A frame on a video or sound channel (not AAP_CONTROL), decrypted: 1
 * when it is part of a media message, which is counted and whose
 * acknowledgement goes into out with its last frame; 0 when it belongs
 * to another message, which the caller assembles and passes to
 * aa_hu_message.  A media message is never assembled: a key frame can
 * be larger than the RP2040 has room for. */
int aa_hu_media_frame(struct aa_hu *h, uint8_t channel, uint8_t flags, const uint8_t *p, uint32_t n);
/* 1 for the channels that carry media (video and sound). */
int aa_hu_media_channel(uint8_t channel);

/* The service discovery response, id first: its length, or -1. */
int aa_hu_discovery(uint8_t *out, uint32_t cap);

#endif
