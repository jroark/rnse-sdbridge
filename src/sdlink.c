/* SDLINK.UF2: the bridge whole, an RP2040 that is an Android phone's USB
 * host on one side and an SD card in a slot of the RNS-E 192 on the
 * other, the phone's video going from one to the other
 * (docs/sdbridge.md, "The phone's video into the slot").
 *
 * The phone's side is SDSTREAM.UF2's (src/sdhandshake.c): accessory
 * mode, Android Auto's version exchange, the TLS handshake with the
 * certificate the build was given, and the session as an 800x480 head
 * unit (src/aa_hu.c).  The card's side is SDBRIDGE.UF2's (src/main.c):
 * the made-up volume (src/volume.c) with LINKTEST.BIN, and now
 * AALINK.BIN, a window onto the phone's video as a stream of blocks
 * (src/aalink.h, src/aastream.c) that sdk/examples/aaviewer reads.
 *
 * The phone's video messages go into the stream as they are decrypted,
 * frame by frame (a key frame can be bigger than the ring: the 192 reads
 * the first blocks while the last come).  A message is acknowledged to
 * the phone when its last block has gone out to the 192, so the phone
 * sends no faster than the 192 takes pictures.  When the ring has no
 * room for another frame, the phone's bytes wait in USB until the 192
 * reads (the "paused" count).  Sound is acknowledged and dropped, as
 * SDSTREAM does.
 *
 *   core 0  the card's command line and nothing else, from RAM (the
 *           command loop of src/main.c): it has 64 of the host's clocks
 *           to answer, and no interrupt is enabled on this core.
 *   core 1  TinyUSB, TLS and the session from flash in its main loop,
 *           and the card's data side in two interrupts that the main
 *           loop never holds up: a new read from core 0 (the inter-core
 *           FIFO) and a block gone to the data state machine (DMA).
 *           Each makes the next block, from RAM.
 *
 *   GP2  CLK (input)      GP3  CMD      GP4  DAT0      (as SDBRIDGE)
 *   GP0  the log, UART0 TX at 115200 baud
 *   the USB port: the phone, through an OTG adapter, with 5 V on VBUS
 *   (pin 40), never the slot's 3.3 V (only the ground is joined)
 *
 * The LED (src/status.h): WAIT for a phone, BUSY while it switches and
 * the handshake runs, WORKING once it is done, OK while the 192 reads
 * video, FAIL for an error (until the phone is unplugged). */
#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/clocks.h"
#include "hardware/sync.h"
#include "hardware/irq.h"
#include "hardware/vreg.h"
#include "hardware/structs/systick.h"
#include "tusb.h"
#include "sd.pio.h"
#include "card.h"
#include "volume.h"
#include "status.h"
#include "aap.h"
#include "aoa.h"
#include "aa_tls.h"
#include "aa_hu.h"
#include "aastream.h"
#include "mbedtls/error.h"
#include "hu_cert.h"

#define CMD_PIN 3
#define DAT0_PIN 4
#ifndef SYS_KHZ
#define SYS_KHZ 250000
#endif
#ifndef AA_MAX_UNACKED
#define AA_MAX_UNACKED 1
#endif
/* The ring of blocks for the 192: what RAM is left (arm-none-eabi-size
 * in sdbridge/build.sh says how much that is). */
#ifndef LINK_BLOCKS
#define LINK_BLOCKS 128
#endif
/* Room for a frame's payload, decrypted: below that, no frame is taken
 * from the phone's bytes until the 192 has read. */
#define FRAME_ROOM (16384 + 64)

#if CLK_PIN != 2
#error "CLK_PIN in sd.pio and the wiring here disagree"
#endif

static const PIO pc = pio0, pd = pio1;
static const uint sm_cmd = 0, sm_dat = 0, sm_led = 1;
static uint off_dat;
static int dma_ch;
static spin_lock_t *lock;
static struct card card;

/* ---- the card's command line (core 0) ------------------------------ */

static volatile uint32_t req_gen, req_sector;
static volatile int req_kind;
static volatile uint32_t slowest, reads;

#define LOGRING 64
static volatile uint32_t logring[LOGRING][2];
static volatile uint32_t log_in, log_out, log_lost;

static void __not_in_flash_func(stop_data)(void)
{
    dma_channel_abort((uint)dma_ch);
    pio_sm_set_enabled(pd, sm_dat, false);
    pio_sm_clear_fifos(pd, sm_dat);
    pio_sm_restart(pd, sm_dat);
    pio_sm_exec(pd, sm_dat, pio_encode_set(pio_pindirs, 0));
    pio_sm_exec(pd, sm_dat, pio_encode_jmp(off_dat + sd_dat_offset_start));
    pio_sm_set_enabled(pd, sm_dat, true);
}

static inline uint32_t __not_in_flash_func(cmd_get)(void)
{
    while (pc->fstat & (1u << (PIO_FSTAT_RXEMPTY_LSB + sm_cmd)))
        ;
    return pc->rxf[sm_cmd];
}

static inline void __not_in_flash_func(cmd_put)(uint32_t w)
{
    while (pc->fstat & (1u << (PIO_FSTAT_TXFULL_LSB + sm_cmd)))
        ;
    pc->txf[sm_cmd] = w;
}

static void __attribute__((noinline)) __not_in_flash_func(command_loop)(void)
{
    struct reply r;
    uint8_t cmd[6];
    int i;
    systick_hw->rvr = 0x00ffffff;
    systick_hw->cvr = 0;
    systick_hw->csr = 5;
    for (;;) {
        uint32_t w0 = cmd_get();
        uint32_t w1 = cmd_get();
        uint32_t t0 = systick_hw->cvr, t;
        card_frame(w0, w1, cmd);
        card_command(&card, cmd, &r);
        if (r.nwords)
            for (i = 0; i < r.nwords; i++)
                cmd_put(r.words[i]);
        else
            cmd_put(0);
        t = (t0 - systick_hw->cvr) & 0x00ffffff;
        if (t > slowest)
            slowest = t;
        if (r.read) {
            uint32_t s = spin_lock_blocking(lock);
            stop_data();
            req_kind = r.read > 0 ? r.read : 0;
            req_sector = r.sector;
            req_gen++;
            spin_unlock(lock, s);
            /* Core 1's data side, told (its FIFO interrupt); a full FIFO
             * has told it already. */
            if (sio_hw->fifo_st & SIO_FIFO_ST_RDY_BITS)
                sio_hw->fifo_wr = req_gen;
        }
        if (r.index != 17 && r.index != 18 && r.index != 12 && r.index != 13) {
            uint32_t n = log_in;
            if (n - log_out < LOGRING) {
                logring[n % LOGRING][0] = (uint32_t)r.index | (r.nwords ? 0x100u : 0) | (card.app ? 0x200u : 0);
                logring[n % LOGRING][1] = (uint32_t)cmd[1] << 24 | (uint32_t)cmd[2] << 16 | (uint32_t)cmd[3] << 8 | cmd[4];
                log_in = n + 1;
            } else {
                log_lost++;
            }
        }
    }
}

/* ---- the card's data side (core 1, interrupts) --------------------- */

static uint8_t ring[LINK_BLOCKS][512] __attribute__((aligned(4)));
static struct aastream stream;
static uint32_t link_sector;          /* AALINK.BIN's first sector */

static void __not_in_flash_func(link_sector_words)(uint32_t index, uint32_t *w)
{
    aastream_sector(&stream, index, w);
}

static uint32_t sector_words[128];
static uint32_t blocks[2][CARD_BLOCK_WORDS];
static uint32_t cur_gen = 0xffffffff;
static int cur_kind;
static uint32_t cur_next;             /* the next sector to make */
static int in_dma = -1;               /* the buffer going out, -1 none */
static uint32_t in_dma_sector;
static int ready_buf = -1;            /* the buffer made and waiting, -1 none */
static uint32_t ready_sector;
static volatile uint32_t blocks_sent;

/* A read of the window (not of AALINK.BIN's control or input sectors,
 * whose reads are no place in the stream: with the input counted in, on
 * a unit, 2026-11-18, a turn of the knob let go of pictures not yet read
 * and set acknowledgements free; the 192 then asked for a key frame and
 * the phone's video began anew). */
static int __not_in_flash_func(window_index)(uint32_t sector, uint32_t *index)
{
    return sector >= link_sector && aastream_in_window(sector - link_sector, index);
}

static void __not_in_flash_func(make)(int b, uint32_t sector)
{
    volume_sector(sector, sector_words);
    card_block(sector_words, blocks[b]);
}

/* The block to the data state machine, unless its read has been stopped
 * or replaced meanwhile. */
static int __not_in_flash_func(start)(uint32_t gen, int b)
{
    uint32_t s = spin_lock_blocking(lock);
    int ok = req_gen == gen;
    if (ok)
        dma_channel_transfer_from_buffer_now((uint)dma_ch, blocks[b], CARD_BLOCK_WORDS);
    spin_unlock(lock, s);
    return ok;
}

/* The next block of a multiple read made, while the last goes out. */
static void __not_in_flash_func(make_next)(void)
{
    if (cur_kind != 2 || ready_buf >= 0 || cur_next >= VOLUME_SECTORS)
        return;
    ready_buf = in_dma == 0 ? 1 : 0;
    ready_sector = cur_next++;
    make(ready_buf, ready_sector);
}

static void __not_in_flash_func(send_ready)(void)
{
    if (ready_buf < 0 || in_dma >= 0)
        return;
    if (!start(cur_gen, ready_buf)) {
        ready_buf = -1;
        cur_kind = 0;
        return;
    }
    in_dma = ready_buf;
    in_dma_sector = ready_sector;
    ready_buf = -1;
    make_next();
}

/* A new read (or a stop) from core 0. */
static void __not_in_flash_func(on_request)(void)
{
    uint32_t s, gen, sector, index;
    int kind;
    while (multicore_fifo_rvalid())
        (void)sio_hw->fifo_rd;
    multicore_fifo_clear_irq();
    s = spin_lock_blocking(lock);
    gen = req_gen;
    kind = req_kind;
    sector = req_sector;
    spin_unlock(lock, s);
    if (gen == cur_gen)
        return;
    cur_gen = gen;
    cur_kind = kind;
    in_dma = ready_buf = -1;
    dma_hw->ints0 = 1u << dma_ch;
    if (!kind)
        return;
    reads++;
    /* A read of the window: the 192 has what came before. */
    if (window_index(sector, &index))
        aastream_consumed(&stream, index);
    ready_buf = 0;
    ready_sector = sector;
    cur_next = sector + 1;
    make(0, sector);
    send_ready();
}

/* A block's words all taken by the data state machine. */
static void __not_in_flash_func(on_dma)(void)
{
    uint32_t index;
    dma_hw->ints0 = 1u << dma_ch;
    /* (An aborted transfer can still raise the interrupt, after the next
     * has begun: not done while the channel is busy.) */
    if (in_dma < 0 || dma_channel_is_busy((uint)dma_ch))
        return;
    blocks_sent++;
    if (window_index(in_dma_sector, &index))
        aastream_went_out(&stream, aastream_seq(&stream, index));
    in_dma = -1;
    if (cur_kind == 1) {
        cur_kind = 0;
        return;
    }
    send_ready();
}

/* ---- the phone (core 1, main loop) --------------------------------- */

static struct aap_reader reader;
static struct aap_assembler msg;
static struct aa_tls tls;
static struct aa_hu hu;
static uint8_t tls_out[AA_TLS_OUT_BUF];
static uint8_t frame[AA_TLS_OUT_BUF + 8];
static uint8_t plain[AAP_FRAME_MAX];

enum { WAITING, SWITCHING, VERSION, HANDSHAKE, AUTHED, STREAMING, FAILED };
static int state = WAITING;
static uint8_t daddr;

/* The video's acknowledgements still to send: the last block of each
 * message. */
#define ACKQ 16
static uint32_t ackq[ACKQ];
static unsigned ack_in, ack_out;
static int video_open;                /* a video message under way in the stream */
static uint32_t dropped_bytes;
static uint32_t key_asked_seen;

/* The phone's bytes not yet made into frames: a USB buffer, taken a
 * piece at a time while the ring has room. */
static const uint8_t *pend;
static uint32_t pend_len, pend_at;
static int paused;

static uint64_t usb_bytes, decrypt_us;
static uint32_t t_auth, t_stats;

static void fail(const char *why, int r)
{
    char text[100];
    if (r) {
        mbedtls_strerror(r, text, sizeof text);
        printf("%s: %s (-0x%04x)\n", why, text, (unsigned)-r);
    } else {
        printf("%s\n", why);
    }
    state = FAILED;
    status_set(ST_FAIL);
}

static uint32_t ms(void)
{
    return to_ms_since_boot(get_absolute_time());
}

static int send_handshake(const uint8_t *p, size_t n)
{
    size_t i, len = n + 2;
    if (!n)
        return 0;
    if (len + 4 > sizeof frame)
        return -1;
    frame[0] = 0;
    frame[1] = AAP_FIRST | AAP_LAST;
    frame[2] = (uint8_t)(len >> 8);
    frame[3] = (uint8_t)len;
    frame[4] = 0x00;
    frame[5] = AAP_SSL_HANDSHAKE;
    for (i = 0; i < n; i++)
        frame[6 + i] = p[i];
    return aoa_send(daddr, frame, (uint32_t)(len + 4));
}

static int send_encrypted(uint8_t channel, uint8_t flags, const uint8_t *p, uint32_t n)
{
    size_t rl;
    int r = aa_tls_encrypt(&tls, p, n, tls_out, sizeof tls_out, &rl);
    if (r) {
        fail("encrypting", r);
        return -1;
    }
    if (aoa_send(daddr, frame, (uint32_t)aap_frame(frame, channel, (uint8_t)(flags | AAP_ENCRYPTED), tls_out, (uint16_t)rl))) {
        fail("sending to the phone failed", 0);
        return -1;
    }
    return 0;
}

static void flush(void)
{
    unsigned i;
    for (i = 0; i < hu.nout && state != FAILED; i++)
        send_encrypted(hu.out[i].channel, hu.out[i].flags, hu.out[i].data, hu.out[i].len);
    hu.nout = 0;
}

static void handshake(const uint8_t *in, size_t n)
{
    size_t out_len;
    uint32_t t0 = ms();
    int r = aa_tls_handshake(&tls, in, n, tls_out, &out_len);
    printf("handshake: %u bytes from the phone, %lu ms on the Pico\n", (unsigned)n, (unsigned long)(ms() - t0));
    if (out_len && send_handshake(tls_out, out_len)) {
        fail("sending to the phone failed", 0);
        return;
    }
    if (r == MBEDTLS_ERR_SSL_FATAL_ALERT_MESSAGE) {
        printf("the phone ended the handshake with alert %d\n", aa_tls_alert(&tls));
        state = FAILED;
        status_set(ST_FAIL);
        return;
    }
    if (r < 0) {
        fail("handshake", r);
        return;
    }
    if (r == 1) {
        uint8_t payload[8];
        int k = aap_auth_complete(payload);
        printf("handshake done: %s\n", aa_tls_suite(&tls));
        if (aoa_send(daddr, frame, (uint32_t)aap_frame(frame, 0, 0, payload, (uint16_t)k))) {
            fail("sending to the phone failed", 0);
            return;
        }
        state = AUTHED;
        t_auth = t_stats = ms();
        status_set(ST_WORKING);
        aa_hu_init(&hu, AA_MAX_UNACKED);
        hu.defer_video_acks = 1;
        stream.state |= AALINK_PHONE;
        stream.width = AA_HU_WIDTH;
        stream.height = AA_HU_HEIGHT;
        ack_in = ack_out = 0;
        key_asked_seen = stream.key_asked;
    }
}

/* A video frame of a media message, decrypted: into the stream. */
static void video_frame(uint8_t flags, const uint8_t *p, uint32_t n)
{
    if (flags & AAP_FIRST) {
        uint16_t id = (uint16_t)(p[0] << 8 | p[1]);
        int type = id == AA_AV_MEDIA ? AALINK_CONFIG : AALINK_PICTURE;
        uint32_t skip = id == AA_AV_MEDIA ? 2 : 10;  /* the id, and the timestamp */
        if (n < skip)
            return;
        p += skip;
        n -= skip;
        aastream_begin(&stream, type, type == AALINK_PICTURE && aastream_is_key(p, n < 64 ? n : 64));
        video_open = 1;
    }
    if (!video_open)
        return;
    {
        uint32_t k = aastream_add(&stream, p, n);
        if (k < n)
            dropped_bytes += n - k;   /* should not happen: room was looked at first */
    }
    if (flags & AAP_LAST) {
        uint32_t last = aastream_end(&stream);
        video_open = 0;
        if (ack_in - ack_out < ACKQ)
            ackq[ack_in++ % ACKQ] = last;
        else
            aa_hu_video_ack(&hu);     /* (more than the phone may send ahead) */
        if (state == AUTHED) {
            state = STREAMING;
            printf("video arriving, %lu ms after authentication\n", (unsigned long)(ms() - t_auth));
        }
    }
}

static void session_frame(const struct aap_frame *f, const uint8_t *p, uint32_t n)
{
    if (aa_hu_media_channel(f->channel) && !(f->flags & AAP_CONTROL) &&
        aa_hu_media_frame(&hu, f->channel, f->flags, p, n)) {
        if (f->channel == AA_CH_VIDEO)
            video_frame(f->flags, p, n);
    } else if (aap_assemble(&msg, f->flags, p, n) == 1) {
        aa_hu_message(&hu, f->channel, f->flags, msg.buf, msg.len);
    }
    flush();
}

static void one_frame(struct aap_frame *f)
{
    uint16_t major, minor, st;
    if (state == VERSION) {
        if (aap_version_response(f, &major, &minor, &st) == 0) {
            printf("Android Auto answered: version %u.%u, %s\n", major, minor, st == 0 ? "they match" : "they do not match");
            if (st != 0) {
                fail("version mismatch", 0);
                return;
            }
            state = HANDSHAKE;
            status_set(ST_BUSY);
            if (aa_tls_init(&tls, 0, hu_cert_pem, sizeof hu_cert_pem, HU_CERT_WIRE ? hu_cert_wire : NULL,
                            HU_CERT_WIRE ? sizeof hu_cert_wire : 0, hu_key_pem, sizeof hu_key_pem, NULL, 0)) {
                fail("the certificate or key the build was given did not parse", 0);
                return;
            }
            handshake(NULL, 0);
        }
        return;
    }
    if (f->flags & AAP_ENCRYPTED) {
        size_t n;
        uint32_t t0 = time_us_32();
        int r = aa_tls_decrypt(&tls, f->payload, f->length, plain, sizeof plain, &n);
        decrypt_us += time_us_32() - t0;
        if (r) {
            fail("decrypting", r);
            return;
        }
        if (state >= AUTHED)
            session_frame(f, plain, (uint32_t)n);
        return;
    }
    if (aap_assemble(&msg, f->flags, f->payload, f->length) != 1)
        return;
    if (state == HANDSHAKE && f->channel == 0 && msg.len >= 2 && msg.buf[0] == 0 && msg.buf[1] == AAP_SSL_HANDSHAKE)
        handshake(msg.buf + 2, msg.len - 2);
}

/* The phone's bytes: frames taken while the ring has room for one. */
static void phone_bytes(void)
{
    struct aap_frame f;
    if (!pend) {
        if (!aoa_in_done)
            return;
        if (aoa_in_done < 0) {
            fail("reading from the phone failed", 0);
            return;
        }
        pend = aoa_in;
        pend_len = aoa_in_len;
        pend_at = 0;
        usb_bytes += pend_len;
        /* The next transfer, into the other buffer, while this one is
         * taken apart. */
        if (aoa_read(daddr)) {
            fail("reading from the phone failed", 0);
            return;
        }
    }
    while (state != FAILED) {
        if (aastream_room(&stream) < FRAME_ROOM) {
            if (!paused) {
                paused = 1;
                stream.paused++;
            }
            return;
        }
        paused = 0;
        if (aap_next(&reader, &f)) {
            one_frame(&f);
            continue;
        }
        if (pend_at >= pend_len) {
            pend = NULL;
            return;
        }
        {
            uint32_t k = pend_len - pend_at < AAP_FEED_MAX ? pend_len - pend_at : AAP_FEED_MAX;
            if (aap_feed(&reader, pend + pend_at, k)) {
                fail("the phone's bytes make no frames", 0);
                return;
            }
            pend_at += k;
        }
    }
}

/* Acknowledgements for what the 192 has read, and a key frame when it
 * asked for one. */
static void for_the_192(void)
{
    static const uint32_t key_codes[AALINK_KEYS] = AALINK_KEY_CODES;
    uint16_t code;
    /* The 192's keys and knob (aalink.h, "Input"), to the phone; with
     * no session, dropped. */
    while (aastream_input_take(&stream, &code)) {
        uint64_t us = time_us_64();
        if (state < AUTHED || state == FAILED)
            continue;
        if (code & AALINK_IN_KNOB) {
            aa_hu_rotary(&hu, us, (int8_t)(code & 0xff));
        } else if (((code & 0xff) >> 1) < AALINK_KEYS) {
            aa_hu_key(&hu, us, key_codes[(code & 0xff) >> 1], code & 1);
        }
        flush();
    }
    if (state < AUTHED || state == FAILED)
        return;
    while (ack_out != ack_in && (int32_t)(stream.sent - (ackq[ack_out % ACKQ] + 1)) >= 0) {
        aa_hu_video_ack(&hu);
        ack_out++;
        /* Each sent at once: the session's queue holds only a few. */
        flush();
    }
    if (stream.key_asked != key_asked_seen) {
        key_asked_seen = stream.key_asked;
        aa_hu_ask_key_frame(&hu);
    }
    flush();
}

static const char *const state_names[] = { "idle", "ready", "ident", "stby", "tran", "data" };

/* The card's commands (but the reads) and once a second the counts. */
static void log_card(void)
{
    static uint32_t last_blocks, last_messages, last_reads;
    static uint64_t last_usb, last_decrypt;
    uint32_t now = ms(), dt;
    while (log_out != log_in) {
        uint32_t k = log_out % LOGRING;
        uint32_t what = logring[k][0], arg = logring[k][1];
        log_out++;
        printf("%sCMD%lu %08lx%s\n", what & 0x200 ? "A" : "", (unsigned long)(what & 0x3f), (unsigned long)arg,
            what & 0x100 ? "" : "  (no response)");
    }
    dt = now - t_stats;
    if (dt < 1000)
        return;
    {
        uint32_t b = blocks_sent, mhz = clock_get_hz(clk_sys) / 1000000;
        int moved = b != last_blocks;
        printf("card %s, %lu reads, %lu KB/s out (slowest answer %lu of the host's clocks, bad CRC %lu)%s; "
               "phone: %lu pictures/s, USB %lu KB/s, decrypting %lu%%; ring %lu of %u blocks, head %lu, "
               "%lu acks, %lu paused, %lu key frames asked, %lu inputs%s\n",
            state_names[card.state], (unsigned long)(reads - last_reads),
            (unsigned long)((b - last_blocks) * 512 / 1024 * 1000 / dt),
            (unsigned long)(slowest * 169 / (mhz * 10) + 1), (unsigned long)card.bad_crc,
            log_lost ? " (log lost some)" : "",
            (unsigned long)((stream.messages - last_messages) * 1000 / dt),
            (unsigned long)((usb_bytes - last_usb) * 1000 / 1024 / dt),
            (unsigned long)((decrypt_us - last_decrypt) / 10 / dt),
            (unsigned long)(stream.head - stream.tail), (unsigned)LINK_BLOCKS, (unsigned long)stream.head,
            (unsigned long)hu.v.acks, (unsigned long)stream.paused, (unsigned long)stream.key_asked,
            (unsigned long)stream.inputs, dropped_bytes ? " (bytes dropped!)" : "");
        last_blocks = b;
        last_reads = reads;
        last_messages = stream.messages;
        last_usb = usb_bytes;
        last_decrypt = decrypt_us;
        t_stats = now;
        if (state == STREAMING)
            status_set(moved ? ST_OK : ST_WORKING);
    }
}

static void phone_gone(void)
{
    if (state >= HANDSHAKE)
        aa_tls_free(&tls);
    if (video_open) {
        (void)aastream_end(&stream);
        video_open = 0;
    }
    stream.state &= (uint16_t)~(AALINK_PHONE | AALINK_VIDEO);
    pend = NULL;
    paused = 0;
    ack_in = ack_out = 0;
    state = WAITING;
    status_set(ST_WAIT);
    printf("the phone is gone\n");
}

static void core1(void)
{
    /* The data side's interrupts, on this core, before USB's. */
    multicore_fifo_clear_irq();
    irq_set_exclusive_handler(SIO_IRQ_PROC1, on_request);
    irq_set_priority(SIO_IRQ_PROC1, 0x40);
    irq_set_enabled(SIO_IRQ_PROC1, true);
    dma_channel_set_irq0_enabled((uint)dma_ch, true);
    irq_set_exclusive_handler(DMA_IRQ_0, on_dma);
    irq_set_priority(DMA_IRQ_0, 0x40);
    irq_set_enabled(DMA_IRQ_0, true);
    tuh_init(0);
    t_stats = ms();
    for (;;) {
        tuh_task();
        status_tick();
        if (!aoa_device && state != WAITING && state != SWITCHING)
            phone_gone();
        if (aoa_fresh) {
            uint16_t vid = 0, pid = 0;
            aoa_fresh = 0;
            daddr = aoa_device;
            tuh_vid_pid_get(daddr, &vid, &pid);
            printf("device %u: %04x:%04x\n", daddr, vid, pid);
            if (aoa_is_accessory(vid, pid)) {
                uint8_t m[16];
                int n = aap_version_request(m);
                aap_reader_init(&reader);
                aap_assembler_init(&msg);
                pend = NULL;
                usb_bytes = decrypt_us = 0;
                if (aoa_open(daddr) || aoa_send(daddr, m, (uint32_t)n) || aoa_read(daddr)) {
                    fail("the accessory's endpoints did not work", 0);
                } else {
                    state = VERSION;
                    status_set(ST_BUSY);
                }
            } else {
                status_set(ST_BUSY);
                if (aoa_switch(daddr))
                    fail("no accessory mode", 0);
                else
                    state = SWITCHING;
            }
        }
        if (state >= VERSION && state != FAILED)
            phone_bytes();
        for_the_192();
        log_card();
    }
}

static uint32_t core1_stack[16384 / 4];

/* ---- set-up -------------------------------------------------------- */

static void setup_pio(void)
{
    uint off_cmd = pio_add_program(pc, &sd_cmd_program);
    pio_sm_config c;
    gpio_init(CLK_PIN);
    gpio_set_dir(CLK_PIN, GPIO_IN);
    gpio_disable_pulls(CLK_PIN);
    for (uint p = DAT0_PIN + 1; p <= DAT0_PIN + 3; p++) {
        gpio_init(p);
        gpio_set_dir(p, GPIO_IN);
        gpio_disable_pulls(p);
    }
    pio_gpio_init(pc, CMD_PIN);
    gpio_disable_pulls(CMD_PIN);
    pio_sm_set_consecutive_pindirs(pc, sm_cmd, CMD_PIN, 1, false);
    c = sd_cmd_program_get_default_config(off_cmd);
    sm_config_set_in_pins(&c, CMD_PIN);
    sm_config_set_out_pins(&c, CMD_PIN, 1);
    sm_config_set_set_pins(&c, CMD_PIN, 1);
    sm_config_set_jmp_pin(&c, CMD_PIN);
    sm_config_set_in_shift(&c, false, true, 32);
    sm_config_set_out_shift(&c, false, true, 32);
    pio_sm_init(pc, sm_cmd, off_cmd + sd_cmd_offset_start, &c);

    off_dat = pio_add_program(pd, &sd_dat_program);
    pio_gpio_init(pd, DAT0_PIN);
    gpio_disable_pulls(DAT0_PIN);
    pio_sm_set_consecutive_pindirs(pd, sm_dat, DAT0_PIN, 1, false);
    c = sd_dat_program_get_default_config(off_dat);
    sm_config_set_out_pins(&c, DAT0_PIN, 1);
    sm_config_set_set_pins(&c, DAT0_PIN, 1);
    sm_config_set_out_shift(&c, false, true, 32);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
    pio_sm_init(pd, sm_dat, off_dat + sd_dat_offset_start, &c);

    dma_ch = dma_claim_unused_channel(true);
    {
        dma_channel_config d = dma_channel_get_default_config((uint)dma_ch);
        channel_config_set_transfer_data_size(&d, DMA_SIZE_32);
        channel_config_set_read_increment(&d, true);
        channel_config_set_write_increment(&d, false);
        channel_config_set_dreq(&d, pio_get_dreq(pd, sm_dat, true));
        dma_channel_configure((uint)dma_ch, &d, &pd->txf[sm_dat], blocks[0], CARD_BLOCK_WORDS, false);
    }
    pio_sm_set_enabled(pd, sm_dat, true);
    pio_sm_set_enabled(pc, sm_cmd, true);
}

int main(void)
{
    vreg_set_voltage(VREG_VOLTAGE_1_15);
    sleep_ms(2);
    set_sys_clock_khz(SYS_KHZ, true);
    stdio_init_all();
    lock = spin_lock_init((uint)spin_lock_claim_unused(true));
    aastream_init(&stream, ring, LINK_BLOCKS);
    link_sector = volume_link_sector();
    volume_link = link_sector_words;
    card_reset(&card);
    setup_pio();
    status_init(pd, sm_led);
    printf("\nsdlink: an SD card of %u MiB with LINKTEST.BIN and AALINK.BIN (the phone's video, %u blocks of RAM), "
           "%lu MHz; waiting for a phone on the USB port (%d frame%s ahead of the acknowledgements)\n",
        VOLUME_SECTORS / 2048, (unsigned)LINK_BLOCKS, (unsigned long)(clock_get_hz(clk_sys) / 1000000), AA_MAX_UNACKED,
        AA_MAX_UNACKED == 1 ? "" : "s");
    multicore_launch_core1_with_stack(core1, core1_stack, sizeof core1_stack);
    /* This core: the command line only, with nothing to interrupt it. */
    command_loop();
}
