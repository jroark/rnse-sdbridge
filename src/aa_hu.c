#include <stdio.h>
#include <string.h>
#include "aap.h"
#include "aa_hu.h"

#define LOG(...) do { if (!h->quiet) printf(__VA_ARGS__); } while (0)

void aa_hu_init(struct aa_hu *h, int max_unacked)
{
    memset(h, 0, sizeof *h);
    h->max_unacked = max_unacked;
    h->session = 0;
    h->v.profile = h->v.constraints = h->v.level = -1;
}

/* A message for the phone, its fields to be written by the caller. */
static struct aap_pb *queue(struct aa_hu *h, uint8_t channel, uint8_t flags, uint16_t id, struct aap_pb *b)
{
    struct aa_hu_msg *m;
    if (h->nout >= AA_HU_QUEUE) {
        static uint8_t spare[AA_HU_MSG_MAX];
        LOG("aa_hu: too many answers at once, %04x dropped\n", id);
        aap_pb_init(b, spare, sizeof spare);
        return b;
    }
    m = &h->out[h->nout++];
    m->channel = channel;
    m->flags = flags;
    m->len = 0;
    aap_pb_init(b, m->data, sizeof m->data);
    aap_pb_id(b, id);
    return b;
}

static void done(struct aa_hu *h, struct aap_pb *b)
{
    if (h->nout && b->p == h->out[h->nout - 1].data)
        h->out[h->nout - 1].len = (uint16_t)b->len;
}

/* One field's value, or `none` when the message has no such field. */
static uint64_t field(const uint8_t *p, uint32_t n, uint32_t number, uint64_t none)
{
    struct aap_pb_field f;
    uint32_t at = 0;
    while (aap_pb_next(p, n, &at, &f) == 1)
        if (f.number == number && f.type == 0)
            return f.value;
    return none;
}

int aa_hu_discovery(uint8_t *out, uint32_t cap)
{
    uint8_t ch[96], body[64], inner[32];
    struct aap_pb b, c, d, e;
    static const uint32_t keys[] = { 0x03, 0x04, 0x13, 0x14, 0x15, 0x16, 0x17, 0x55, 0x57, 0x58, 65536 };
    unsigned i;
    aap_pb_init(&b, out, cap);
    aap_pb_id(&b, AAP_SERVICE_DISCOVERY_RESPONSE);

    /* Sensors: driving status (13) and night mode (10). */
    aap_pb_init(&d, body, sizeof body);
    aap_pb_init(&e, inner, sizeof inner);
    aap_pb_varint(&e, 1, 13);
    aap_pb_bytes(&d, 1, inner, e.len);
    aap_pb_init(&e, inner, sizeof inner);
    aap_pb_varint(&e, 1, 10);
    aap_pb_bytes(&d, 1, inner, e.len);
    aap_pb_init(&c, ch, sizeof ch);
    aap_pb_varint(&c, 1, AA_CH_SENSOR);
    aap_pb_bytes(&c, 2, body, d.len);
    aap_pb_bytes(&b, 1, ch, c.len);
    if (d.err || e.err || c.err)
        return -1;

    /* Video: stream type 3, one configuration: 480p (800x480), 30 frames
     * a second (AA_HU_FPS), no margins, 140 dots an inch. */
    aap_pb_init(&e, inner, sizeof inner);
    aap_pb_varint(&e, 1, 1);
    aap_pb_varint(&e, 2, AA_HU_FPS);
    aap_pb_varint(&e, 3, 0);
    aap_pb_varint(&e, 4, 0);
    aap_pb_varint(&e, 5, 140);
    aap_pb_init(&d, body, sizeof body);
    aap_pb_varint(&d, 1, 3);
    aap_pb_bytes(&d, 4, inner, e.len);
    aap_pb_varint(&d, 5, 1);
    aap_pb_init(&c, ch, sizeof ch);
    aap_pb_varint(&c, 1, AA_CH_VIDEO);
    aap_pb_bytes(&c, 3, body, d.len);
    aap_pb_bytes(&b, 1, ch, c.len);
    if (d.err || e.err || c.err)
        return -1;

    /* Input: a few keys and an 800x480 touch screen. */
    aap_pb_init(&d, body, sizeof body);
    for (i = 0; i < sizeof keys / sizeof keys[0]; i++)
        aap_pb_varint(&d, 1, keys[i]);
    aap_pb_init(&e, inner, sizeof inner);
    aap_pb_varint(&e, 1, AA_HU_WIDTH);
    aap_pb_varint(&e, 2, AA_HU_HEIGHT);
    aap_pb_bytes(&d, 2, inner, e.len);
    aap_pb_init(&c, ch, sizeof ch);
    aap_pb_varint(&c, 1, AA_CH_INPUT);
    aap_pb_bytes(&c, 4, body, d.len);
    aap_pb_bytes(&b, 1, ch, c.len);
    if (d.err || e.err || c.err)
        return -1;

    /* Sound: media 48 kHz stereo, speech and system 16 kHz mono (stream
     * type 1, audio, with its audio type), as aasdk's head unit offers. */
    {
        static const struct { uint8_t channel, type; uint32_t rate, channels; } sinks[] = {
            { AA_CH_MEDIA_AUDIO, 3, 48000, 2 },
            { AA_CH_SPEECH_AUDIO, 1, 16000, 1 },
            { AA_CH_SYSTEM_AUDIO, 2, 16000, 1 },
        };
        for (i = 0; i < sizeof sinks / sizeof sinks[0]; i++) {
            aap_pb_init(&e, inner, sizeof inner);
            aap_pb_varint(&e, 1, sinks[i].rate);
            aap_pb_varint(&e, 2, 16);
            aap_pb_varint(&e, 3, sinks[i].channels);
            aap_pb_init(&d, body, sizeof body);
            aap_pb_varint(&d, 1, 1);
            aap_pb_varint(&d, 2, sinks[i].type);
            aap_pb_bytes(&d, 3, inner, e.len);
            aap_pb_init(&c, ch, sizeof ch);
            aap_pb_varint(&c, 1, sinks[i].channel);
            aap_pb_bytes(&c, 3, body, d.len);
            aap_pb_bytes(&b, 1, ch, c.len);
            if (d.err || e.err || c.err)
                return -1;
        }
    }

    /* The microphone: audio, 16 kHz mono. */
    aap_pb_init(&e, inner, sizeof inner);
    aap_pb_varint(&e, 1, 16000);
    aap_pb_varint(&e, 2, 16);
    aap_pb_varint(&e, 3, 1);
    aap_pb_init(&d, body, sizeof body);
    aap_pb_varint(&d, 1, 1);
    aap_pb_bytes(&d, 2, inner, e.len);
    aap_pb_init(&c, ch, sizeof ch);
    aap_pb_varint(&c, 1, AA_CH_MIC);
    aap_pb_bytes(&c, 5, body, d.len);
    aap_pb_bytes(&b, 1, ch, c.len);
    if (d.err || e.err || c.err)
        return -1;

    aap_pb_string(&b, 2, "RNS-E");        /* head unit name */
    aap_pb_string(&b, 3, "RNS-E");        /* car model */
    aap_pb_string(&b, 4, "2006");         /* car year */
    aap_pb_string(&b, 5, "0001");         /* car serial */
    aap_pb_varint(&b, 6, 1);              /* left-hand drive */
    aap_pb_string(&b, 7, "rnse");         /* head unit manufacturer */
    aap_pb_string(&b, 8, "sdbridge");     /* head unit model */
    aap_pb_string(&b, 9, "1");            /* software build */
    aap_pb_string(&b, 10, "1.0");         /* software version */
    aap_pb_varint(&b, 11, 0);             /* native media during voice */
    aap_pb_varint(&b, 12, 0);             /* hide the clock */
    return b.err ? -1 : (int)b.len;
}

static void focus_as(struct aa_hu *h, int mode)
{
    struct aap_pb b;
    queue(h, AA_CH_VIDEO, 0, AA_VIDEO_FOCUS_INDICATION, &b);
    aap_pb_varint(&b, 1, (uint64_t)mode);  /* 1 focused (projected), 2 not (the head unit's own) */
    aap_pb_varint(&b, 2, 0);  /* not unrequested */
    done(h, &b);
}

static void focus(struct aa_hu *h)
{
    focus_as(h, 1);
}

void aa_hu_ask_key_frame(struct aa_hu *h)
{
    LOG("asking for a key frame: video focus away and back\n");
    focus_as(h, 2);
    focus_as(h, 1);
}

/* An input event: the timestamp (microseconds), then the event as field
 * 4 (buttons) or 6 (relative input), each a list of one, as aasdk's
 * InputEventIndication, ButtonEvents/ButtonEvent (scan code 1, pressed
 * 2, meta 3, long press 4) and RelativeInputEvents/RelativeInputEvent
 * (scan code 1, delta 2) have them. */
static void input_event(struct aa_hu *h, uint64_t us, int field, const uint8_t *event, uint32_t n)
{
    struct aap_pb b, list;
    uint8_t buf[32];
    aap_pb_init(&list, buf, sizeof buf);
    aap_pb_bytes(&list, 1, event, n);
    queue(h, AA_CH_INPUT, 0, AA_INPUT_EVENT, &b);
    aap_pb_varint(&b, 1, us);
    aap_pb_bytes(&b, (uint32_t)field, buf, list.len);
    done(h, &b);
}

void aa_hu_key(struct aa_hu *h, uint64_t us, uint32_t code, int pressed)
{
    struct aap_pb e;
    uint8_t ev[16];
    aap_pb_init(&e, ev, sizeof ev);
    aap_pb_varint(&e, 1, code);
    aap_pb_varint(&e, 2, pressed ? 1 : 0);
    aap_pb_varint(&e, 3, 0);
    aap_pb_varint(&e, 4, 0);
    input_event(h, us, 4, ev, e.len);
    h->inputs++;
}

void aa_hu_rotary(struct aa_hu *h, uint64_t us, int32_t delta)
{
    struct aap_pb e;
    uint8_t ev[24];
    aap_pb_init(&e, ev, sizeof ev);
    aap_pb_varint(&e, 1, 65536);                       /* the rotary controller */
    aap_pb_varint(&e, 2, (uint64_t)(int64_t)delta);    /* int32: a negative one as ten bytes */
    input_event(h, us, 6, ev, e.len);
    h->inputs++;
}

void aa_hu_video_ack(struct aa_hu *h)
{
    struct aap_pb b;
    h->v.acks++;
    queue(h, AA_CH_VIDEO, 0, AA_AV_ACK, &b);
    aap_pb_varint(&b, 1, (uint64_t)(int64_t)h->session);
    aap_pb_varint(&b, 2, 1);
    done(h, &b);
}

static int control(struct aa_hu *h, uint8_t channel, uint16_t id, const uint8_t *p, uint32_t n)
{
    struct aap_pb b;
    struct aap_pb_field f;
    uint32_t at = 0;
    uint64_t v;
    switch (id) {
    case AAP_SERVICE_DISCOVERY_REQUEST:
        LOG("the phone's service discovery request:");
        while (aap_pb_next(p, n, &at, &f) == 1)
            if (f.type == 2 && (f.number == 4 || f.number == 5))
                LOG(" %s \"%.*s\"", f.number == 4 ? "name" : "brand", (int)f.len, f.bytes);
        LOG("\n");
        if (h->nout < AA_HU_QUEUE) {
            struct aa_hu_msg *m = &h->out[h->nout];
            int k = aa_hu_discovery(m->data, sizeof m->data);
            if (k > 0) {
                m->channel = AA_CH_CONTROL;
                m->flags = 0;
                m->len = (uint16_t)k;
                h->nout++;
                LOG("service discovery response: %d bytes, channels input %d, sensors %d, video %d (%dx%d), "
                    "sound %d-%d, microphone %d\n", k, AA_CH_INPUT, AA_CH_SENSOR, AA_CH_VIDEO, AA_HU_WIDTH,
                    AA_HU_HEIGHT, AA_CH_MEDIA_AUDIO, AA_CH_SYSTEM_AUDIO, AA_CH_MIC);
            }
        }
        return 1;
    case AAP_CHANNEL_OPEN_REQUEST: {
        int ok = channel >= AA_CH_INPUT && channel <= AA_CH_MIC;
        LOG("channel %d open (priority %d): %s\n", channel, (int)field(p, n, 1, 0), ok ? "ok" : "refused");
        queue(h, channel, AAP_CONTROL, AAP_CHANNEL_OPEN_RESPONSE, &b);
        aap_pb_varint(&b, 1, ok ? 0 : 1);
        done(h, &b);
        return 1;
    }
    case AAP_PING_REQUEST:
        h->pings++;
        queue(h, AA_CH_CONTROL, 0, AAP_PING_RESPONSE, &b);
        aap_pb_varint(&b, 1, field(p, n, 1, 0));
        done(h, &b);
        return 1;
    case AAP_NAV_FOCUS_REQUEST:
        LOG("navigation focus request (type %d)\n", (int)field(p, n, 1, 0));
        queue(h, AA_CH_CONTROL, 0, AAP_NAV_FOCUS_RESPONSE, &b);
        aap_pb_varint(&b, 1, 2);
        done(h, &b);
        return 1;
    case AAP_AUDIO_FOCUS_REQUEST:
        /* Release: loss; anything else: gain (as aasdk's head unit). */
        v = field(p, n, 1, 0);
        LOG("audio focus request (type %d)\n", (int)v);
        queue(h, AA_CH_CONTROL, 0, AAP_AUDIO_FOCUS_RESPONSE, &b);
        aap_pb_varint(&b, 1, v == 4 ? 3 : 1);
        done(h, &b);
        return 1;
    case AAP_SHUTDOWN_REQUEST:
        LOG("the phone asks to shut down (reason %d)\n", (int)field(p, n, 1, 0));
        queue(h, AA_CH_CONTROL, 0, AAP_SHUTDOWN_RESPONSE, &b);
        done(h, &b);
        return 1;
    case AAP_VOICE_SESSION_REQUEST:
        LOG("voice session request\n");
        return 1;
    }
    return 0;
}

static int sensor(struct aa_hu *h, uint16_t id, const uint8_t *p, uint32_t n)
{
    struct aap_pb b;
    uint8_t inner[8];
    struct aap_pb e;
    uint64_t type;
    if (id != AA_SENSOR_START_REQUEST)
        return 0;
    type = field(p, n, 1, 0);
    LOG("sensor start: type %d%s\n", (int)type, type == 13 ? " (driving status)" : type == 10 ? " (night mode)" : "");
    queue(h, AA_CH_SENSOR, 0, AA_SENSOR_START_RESPONSE, &b);
    aap_pb_varint(&b, 1, 0);
    done(h, &b);
    if (type == 13 || type == 10) {
        /* Its first value: unrestricted, or day. */
        aap_pb_init(&e, inner, sizeof inner);
        aap_pb_varint(&e, 1, 0);
        queue(h, AA_CH_SENSOR, 0, AA_SENSOR_EVENT, &b);
        aap_pb_bytes(&b, (uint32_t)type, inner, e.len);
        done(h, &b);
    }
    return 1;
}

static int input(struct aa_hu *h, uint16_t id)
{
    struct aap_pb b;
    if (id != AA_INPUT_BINDING_REQUEST)
        return 0;
    LOG("input binding request\n");
    queue(h, AA_CH_INPUT, 0, AA_INPUT_BINDING_RESPONSE, &b);
    aap_pb_varint(&b, 1, 0);
    done(h, &b);
    return 1;
}

static int video(struct aa_hu *h, uint16_t id, const uint8_t *p, uint32_t n)
{
    struct aap_pb b;
    switch (id) {
    case AA_AV_SETUP_REQUEST:
        LOG("video setup request (configuration %d): ok, %d frames ahead of the acknowledgements\n",
            (int)field(p, n, 1, 0), h->max_unacked);
        queue(h, AA_CH_VIDEO, 0, AA_AV_SETUP_RESPONSE, &b);
        aap_pb_varint(&b, 1, 2);           /* ok */
        aap_pb_varint(&b, 2, (uint64_t)h->max_unacked);
        aap_pb_varint(&b, 3, 0);           /* configuration 0 */
        done(h, &b);
        focus(h);
        return 1;
    case AA_AV_START:
        h->session = (int)field(p, n, 1, 0);
        h->video_started = 1;
        LOG("video start: session %d, configuration %d\n", h->session, (int)field(p, n, 2, 0));
        return 1;
    case AA_AV_STOP:
        h->video_started = 0;
        LOG("video stop\n");
        return 1;
    case AA_VIDEO_FOCUS_REQUEST:
        LOG("video focus request (mode %d, reason %d)\n", (int)field(p, n, 2, 0), (int)field(p, n, 3, 0));
        focus(h);
        return 1;
    }
    return 0;
}

static int sound(struct aa_hu *h, uint8_t channel, uint16_t id, const uint8_t *p, uint32_t n)
{
    struct aap_pb b;
    switch (id) {
    case AA_AV_SETUP_REQUEST:
        LOG("sound channel %d setup request: ok\n", channel);
        queue(h, channel, 0, AA_AV_SETUP_RESPONSE, &b);
        aap_pb_varint(&b, 1, 2);           /* ok */
        aap_pb_varint(&b, 2, (uint64_t)h->max_unacked);
        aap_pb_varint(&b, 3, 0);
        done(h, &b);
        return 1;
    case AA_AV_START:
        h->ch[channel].session = (int)field(p, n, 1, 0);
        LOG("sound channel %d start: session %d\n", channel, h->ch[channel].session);
        return 1;
    case AA_AV_STOP:
        LOG("sound channel %d stop\n", channel);
        return 1;
    }
    return 0;
}

static int mic(struct aa_hu *h, uint16_t id, const uint8_t *p, uint32_t n)
{
    struct aap_pb b;
    if (id != AA_AV_INPUT_OPEN_REQUEST)
        return 0;
    /* Answered; nothing is ever recorded or sent. */
    LOG("microphone %s\n", field(p, n, 1, 0) ? "opened (nothing will be sent)" : "closed");
    queue(h, AA_CH_MIC, 0, AA_AV_INPUT_OPEN_RESPONSE, &b);
    aap_pb_varint(&b, 1, 0);
    aap_pb_varint(&b, 2, 0);
    done(h, &b);
    return 1;
}

int aa_hu_media_channel(uint8_t channel)
{
    return channel >= AA_CH_VIDEO && channel <= AA_CH_SYSTEM_AUDIO;
}

int aa_hu_message(struct aa_hu *h, uint8_t channel, uint8_t flags, const uint8_t *m, uint32_t n)
{
    uint16_t id;
    int known = 0;
    if (n < 2)
        return 0;
    id = (uint16_t)(m[0] << 8 | m[1]);
    if (channel == AA_CH_CONTROL || (flags & AAP_CONTROL))
        known = control(h, channel, id, m + 2, n - 2);
    else if (channel == AA_CH_SENSOR)
        known = sensor(h, id, m + 2, n - 2);
    else if (channel == AA_CH_INPUT)
        known = input(h, id);
    else if (aa_hu_media_channel(channel) && (id == AA_AV_MEDIA_TIMESTAMP || id == AA_AV_MEDIA))
        /* A whole media message: as its frames would be. */
        return aa_hu_media_frame(h, channel, AAP_FIRST | AAP_LAST, m, n);
    else if (channel == AA_CH_VIDEO)
        known = video(h, id, m + 2, n - 2);
    else if (aa_hu_media_channel(channel))
        known = sound(h, channel, id, m + 2, n - 2);
    else if (channel == AA_CH_MIC)
        known = mic(h, id, m + 2, n - 2);
    if (!known)
        LOG("message %04x on channel %d (flags %02x), %lu bytes: not handled\n", id, channel, flags, (unsigned long)n);
    return known;
}

/* The NAL units whose start codes are in p: key frames counted, the
 * sequence parameter set's profile and level kept. */
static void nals(struct aa_hu *h, const uint8_t *p, uint32_t n, int *idr)
{
    uint32_t i;
    for (i = 0; i + 3 < n; i++) {
        if (p[i] || p[i + 1] || p[i + 2] != 1)
            continue;
        i += 3;
        switch (p[i] & 0x1f) {
        case 5:
            *idr = 1;
            break;
        case 7:
            if (i + 3 < n) {
                h->v.profile = p[i + 1];
                h->v.constraints = p[i + 2];
                h->v.level = p[i + 3];
            }
            break;
        }
    }
}

int aa_hu_media_frame(struct aa_hu *h, uint8_t channel, uint8_t flags, const uint8_t *p, uint32_t n)
{
    struct aa_hu_video *v = &h->v;
    int video = channel == AA_CH_VIDEO;
    if (channel >= AA_CHANNELS || !aa_hu_media_channel(channel))
        return 0;
    if (flags & AAP_FIRST) {
        uint16_t id;
        int idr = 0;
        if (n < 2)
            return 0;
        id = (uint16_t)(p[0] << 8 | p[1]);
        if (id != AA_AV_MEDIA_TIMESTAMP && id != AA_AV_MEDIA) {
            h->ch[channel].in_media = 0;
            return 0;
        }
        h->ch[channel].in_media = 1;
        h->ch[channel].msg_bytes = 0;
        if (video && id == AA_AV_MEDIA) {
            /* The codec's configuration: its parameter sets. */
            uint32_t k = n - 2 < sizeof v->config ? n - 2 : sizeof v->config;
            memcpy(v->config, p + 2, k);
            v->config_len = k;
            v->configs++;
            v->config_new = 1;
            nals(h, p + 2, n - 2, &idr);
        } else if (video && n > 10) {
            /* After the timestamp: only the first NAL units matter. */
            nals(h, p + 10, n - 10 < 64 ? n - 10 : 64, &idr);
        }
        if (idr)
            v->idr++;
    } else if (!h->ch[channel].in_media) {
        return 0;
    }
    h->ch[channel].msg_bytes += n;
    if (video)
        v->bytes += n;
    else
        h->audio_bytes += n;
    if (flags & AAP_LAST) {
        struct aap_pb b;
        uint32_t len = h->ch[channel].msg_bytes;
        h->ch[channel].in_media = 0;
        if (video) {
            v->messages++;
            if (len > v->largest)
                v->largest = len;
            if (h->defer_video_acks)
                return 1;
            v->acks++;
        } else {
            h->audio_messages++;
        }
        queue(h, channel, 0, AA_AV_ACK, &b);
        aap_pb_varint(&b, 1, (uint64_t)(int64_t)(video ? h->session : h->ch[channel].session));
        aap_pb_varint(&b, 2, 1);
        done(h, &b);
    }
    return 1;
}
