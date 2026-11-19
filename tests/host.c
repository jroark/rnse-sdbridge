/* The bridge's card, volume and checksums on the host, with no RP2040:
 *
 *   sh sdbridge/tests/run.sh
 *
 * Commands are sent as sdk/src/sd.c and sdk/examples/linkbench send
 * them, as frames of six bytes, and the answers are taken apart as a
 * host takes them. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/card.h"
#include "../src/crc.h"
#include "../src/volume.h"
#include "../src/aap.h"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("%s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static struct card card;

/* A command frame, CRC7 and end bit included. */
static void frame(uint8_t index, uint32_t arg, uint8_t *b)
{
    b[0] = (uint8_t)(0x40 | index);
    b[1] = (uint8_t)(arg >> 24);
    b[2] = (uint8_t)(arg >> 16);
    b[3] = (uint8_t)(arg >> 8);
    b[4] = (uint8_t)arg;
    b[5] = (uint8_t)(crc7(b, 5) << 1 | 1);
}

/* The response's bytes out of the reply's words; their number. */
static int bytes(const struct reply *r, uint8_t *b)
{
    int n, i;
    if (!r->nwords)
        return 0;
    n = (int)(r->words[0] + 1) / 8;
    for (i = 0; i < n; i++)
        b[i] = (uint8_t)(r->words[1 + i / 4] >> (24 - 8 * (i % 4)));
    return n;
}

/* A command, through the same two words the command state machine
 * pushes; the response's bytes in b, their number returned. */
static int send(uint8_t index, uint32_t arg, struct reply *r, uint8_t *b)
{
    uint8_t f[6], back[6];
    uint64_t bits = 0;
    uint32_t w0, w1;
    int i;
    frame(index, arg, f);
    for (i = 0; i < 6; i++)
        bits = bits << 8 | f[i];
    /* Bits 46-15, and 14-0 at the top of the second word. */
    w0 = (uint32_t)(bits >> 15);
    w1 = (uint32_t)(bits & 0x7fff) << 17;
    card_frame(w0, w1, back);
    CHECK(memcmp(f, back, 6) == 0);
    card_command(&card, back, r);
    return bytes(r, b);
}

static uint32_t r32(const uint8_t *b)
{
    return (uint32_t)b[1] << 24 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 8 | b[4];
}

/* A short response as a host checks it: index, CRC, end bit. */
static int short_ok(const uint8_t *b, int n, uint8_t index, int crc)
{
    if (n != 6 || (b[0] & 0xc0) != 0 || (b[0] & 0x3f) != index || !(b[5] & 1))
        return 0;
    return !crc || crc7(b, 5) == b[5] >> 1;
}

static uint32_t bits_of(const uint8_t *reg, int hi, int lo)
{
    uint32_t v = 0;
    int b;
    for (b = hi; b >= lo; b--)
        v = v << 1 | ((reg[15 - b / 8] >> (b % 8)) & 1);
    return v;
}

static void test_crc(void)
{
    uint8_t f[6];
    uint32_t ones[128];
    int i;
    /* The SD specification's examples. */
    frame(0, 0, f);
    CHECK(f[5] == 0x95);
    frame(8, 0x1aa, f);
    CHECK(f[5] == 0x87);
    frame(17, 0, f);
    CHECK(f[5] == 0x55);
    for (i = 0; i < 128; i++)
        ones[i] = 0xffffffff;
    CHECK(crc16_words(ones, 128) == 0x7fa1);
}

static void test_start(void)
{
    struct reply r;
    uint8_t b[17];
    uint32_t rca, ocr, c_size, mult, len;
    int n, i;
    card_reset(&card);
    n = send(0, 0, &r, b);
    CHECK(n == 0);
    n = send(8, 0x1aa, &r, b);
    CHECK(short_ok(b, n, 8, 1) && (r32(b) & 0xfff) == 0x1aa);
    /* The first ACMD41 busy, the second ready; standard capacity. */
    for (i = 0; i < 2; i++) {
        n = send(55, 0, &r, b);
        CHECK(short_ok(b, n, 55, 1) && (r32(b) & 0x20));
        n = send(41, 0x40ff0000, &r, b);
        CHECK(n == 6 && (b[0] & 0x3f) == 0x3f && b[5] == 0xff);
        ocr = r32(b);
        CHECK(!!(ocr & 0x80000000) == (i == 1));
        CHECK(!(ocr & 0x40000000));
    }
    n = send(2, 0, &r, b);
    CHECK(n == 17 && b[0] == 0x3f && crc7(b + 1, 15) == b[16] >> 1 && (b[16] & 1));
    n = send(3, 0, &r, b);
    CHECK(short_ok(b, n, 3, 1));
    rca = r32(b) >> 16;
    CHECK(rca != 0);
    CHECK(((r32(b) >> 9) & 15) == IDENT);
    n = send(9, rca << 16, &r, b);
    CHECK(n == 17 && crc7(b + 1, 15) == b[16] >> 1);
    c_size = bits_of(b + 1, 73, 62);
    mult = bits_of(b + 1, 49, 47);
    len = bits_of(b + 1, 83, 80);
    CHECK(((c_size + 1) << (mult + 2)) * (1u << len) == VOLUME_SECTORS * 512);
    CHECK(bits_of(b + 1, 127, 126) == 0);
    CHECK(bits_of(b + 1, 12, 12) == 1);
    n = send(10, rca << 16, &r, b);
    CHECK(n == 17 && memcmp(b + 1, card_cid, 16) == 0);
    /* Not this card's address: no answer. */
    n = send(9, (rca + 1) << 16, &r, b);
    CHECK(n == 0);
    n = send(7, rca << 16, &r, b);
    CHECK(short_ok(b, n, 7, 1) && ((r32(b) >> 9) & 15) == STBY);
    CHECK(!(r32(b) & 0xfff80000));
    CHECK(card.state == TRAN);
    n = send(16, 512, &r, b);
    CHECK(short_ok(b, n, 16, 1) && ((r32(b) >> 9) & 15) == TRAN && (r32(b) & 0x100));
    n = send(13, rca << 16, &r, b);
    CHECK(short_ok(b, n, 13, 1) && !(r32(b) & 0xfff80000));
}

static void test_reads(void)
{
    struct reply r;
    uint8_t b[17];
    int n;
    n = send(17, 1234 * 512, &r, b);
    CHECK(short_ok(b, n, 17, 1) && r.read == 1 && r.sector == 1234);
    CHECK(card.state == TRAN);
    n = send(18, 77 * 512, &r, b);
    CHECK(short_ok(b, n, 18, 1) && r.read == 2 && r.sector == 77);
    CHECK(card.state == DATA);
    n = send(13, (uint32_t)card.rca << 16, &r, b);
    CHECK(((r32(b) >> 9) & 15) == DATA);
    n = send(12, 0, &r, b);
    CHECK(short_ok(b, n, 12, 1) && r.read == -1 && card.state == TRAN);
    /* Out of the card, or not on a sector: no data. */
    n = send(17, VOLUME_SECTORS * 512, &r, b);
    CHECK(short_ok(b, n, 17, 1) && r.read == 0 && (r32(b) & 0x80000000u));
    n = send(17, 100, &r, b);
    CHECK(short_ok(b, n, 17, 1) && r.read == 0 && (r32(b) & 0x40000000u));
    /* Writes: write-protected. */
    n = send(24, 0, &r, b);
    CHECK(short_ok(b, n, 24, 1) && (r32(b) & (1u << 26)) && card.state == TRAN);
    n = send(25, 0, &r, b);
    CHECK(short_ok(b, n, 25, 1) && (r32(b) & (1u << 26)));
}

static void test_damaged(void)
{
    struct reply r;
    uint8_t f[6], b[17];
    int n;
    frame(13, (uint32_t)card.rca << 16, f);
    f[5] ^= 2;
    card_command(&card, f, &r);
    CHECK(r.nwords == 0 && card.bad_crc == 1);
    n = send(13, (uint32_t)card.rca << 16, &r, b);
    CHECK(short_ok(b, n, 13, 1) && (r32(b) & (1u << 23)));
    /* A response's own bits (transmission bit 0) are no command. */
    frame(13, 0, f);
    f[0] &= 0x3f;
    f[5] = (uint8_t)(crc7(f, 5) << 1 | 1);
    card_command(&card, f, &r);
    CHECK(r.nwords == 0);
    /* An unknown command: refused, said with the next. */
    n = send(5, 0, &r, b);
    CHECK(n == 0);
    n = send(13, (uint32_t)card.rca << 16, &r, b);
    CHECK(r32(b) & (1u << 22));
}

static void test_block(void)
{
    uint32_t s[128], out[CARD_BLOCK_WORDS];
    uint16_t c = 0;
    int i, k;
    volume_sector(volume_file_sector() + 3, s);
    card_block(s, out);
    CHECK(out[0] == 4111);
    CHECK(memcmp(out + 1, s, 512) == 0);
    CHECK((out[129] & 0xffff) == 0);
    /* The CRC16 bit by bit. */
    for (i = 0; i < 128; i++)
        for (k = 31; k >= 0; k--) {
            int in = (int)(s[i] >> k) & 1;
            int top = (c >> 15) & 1;
            c = (uint16_t)(c << 1);
            if (in ^ top)
                c ^= 0x1021;
        }
    CHECK(out[129] >> 16 == c);
}

/* The volume, read as sdk/src/sd.c reads one. */
static uint8_t sec[512];
static void read_sector(uint32_t n)
{
    uint32_t w[128];
    int i;
    volume_sector(n, w);
    for (i = 0; i < 512; i++)
        sec[i] = (uint8_t)(w[i / 4] >> (24 - 8 * (i % 4)));
}
static uint32_t le16(const uint8_t *p) { return (uint32_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return le16(p) | le16(p + 2) << 16; }

static void test_volume(void)
{
    uint32_t first, fat_start, root_start, data_start, per_cluster, clusters, c, size = 0, cluster = 0, i, j;
    int e, found = 0;
    read_sector(0);
    CHECK(sec[510] == 0x55 && sec[511] == 0xaa && sec[446 + 4] == 6);
    first = le32(sec + 446 + 8);
    CHECK(first + le32(sec + 446 + 12) == VOLUME_SECTORS);
    read_sector(first);
    CHECK(sec[0] == 0xeb && le16(sec + 11) == 512 && sec[510] == 0x55 && sec[511] == 0xaa);
    per_cluster = sec[13];
    fat_start = first + le16(sec + 14);
    root_start = fat_start + sec[16] * le16(sec + 22);
    data_start = root_start + (le16(sec + 17) * 32 + 511) / 512;
    clusters = (le32(sec + 32) - (data_start - first)) / per_cluster;
    CHECK(clusters >= 4085 && clusters < 65525);
    CHECK((clusters + 2) * 2 <= le16(sec + 22) * 512u);
    CHECK(data_start == volume_file_sector());
    read_sector(root_start);
    for (e = 0; e < 512; e += 32)
        if (memcmp(sec + e, "LINKTESTBIN", 11) == 0 && !(sec[e + 11] & 0x18)) {
            cluster = le16(sec + e + 26);
            size = le32(sec + e + 28);
            found = 1;
        }
    CHECK(found && size == VOLUME_FILE_BYTES && cluster == 2);
    /* The chain: contiguous, and as long as the file. */
    for (c = cluster, i = 1;; i++) {
        uint32_t next;
        read_sector(fat_start + c * 2 / 512);
        next = le16(sec + (c * 2) % 512);
        if (next >= 0xfff8)
            break;
        CHECK(next == c + 1);
        c = next;
        if (i > 100000)
            break;
    }
    CHECK(i * per_cluster * 512 == VOLUME_FILE_BYTES);
    /* Both FATs alike. */
    for (i = 0; i < le16(sec + 22); i++) {
        uint32_t a[128], b[128];
        volume_sector(fat_start + i, a);
        volume_sector(fat_start + 64 + i, b);
        CHECK(memcmp(a, b, 512) == 0);
    }
    /* The file's words, and nothing after it. */
    for (j = 0; j < VOLUME_FILE_BYTES / 512; j += 517) {
        uint32_t w[128];
        volume_sector(data_start + j, w);
        CHECK(w[0] == volume_pattern(j * 128) && w[127] == volume_pattern(j * 128 + 127));
    }
    {
        uint32_t w[128];
        volume_sector(data_start + VOLUME_FILE_BYTES / 512, w);
        CHECK(w[0] == 0 && w[127] == 0);
    }
}

/* LINKBENC.BIN: absent until one is sent, then in the directory with a
 * chain of its own, its bytes as they were sent. */
static int find_program(uint32_t root_start, uint32_t *cluster, uint32_t *size)
{
    int e;
    read_sector(root_start);
    for (e = 0; e < 512; e += 32)
        if (memcmp(sec + e, "LINKBENCBIN", 11) == 0) {
            *cluster = le16(sec + e + 26);
            *size = le32(sec + e + 28);
            return 1;
        }
    return 0;
}

static void test_program_file(void)
{
    const uint32_t fat_start = 33, root_start = 33 + 128;
    uint32_t cluster = 0, size = 0, c, i, n = 9000;
    CHECK(!find_program(root_start, &cluster, &size));
    for (i = 0; i < n; i++)
        volume_program[i] = (uint8_t)(i * 7 + 3);
    volume_program_bytes = n;
    CHECK(find_program(root_start, &cluster, &size) && size == n);
    for (c = cluster, i = 1;; i++) {
        uint32_t next;
        read_sector(fat_start + c * 2 / 512);
        next = le16(sec + (c * 2) % 512);
        if (next >= 0xfff8)
            break;
        CHECK(next == c + 1);
        c = next;
        if (i > 100)
            break;
    }
    CHECK(i == 3);                          /* 9000 bytes: 3 clusters of 4 KiB */
    CHECK(volume_file_sector() + (cluster - 2) * 8 == volume_program_sector());
    for (i = 0; i < n; i += 512) {
        uint32_t k;
        read_sector(volume_program_sector() + i / 512);
        for (k = 0; k < 512 && i + k < n; k++)
            CHECK(sec[k] == (uint8_t)((i + k) * 7 + 3));
    }
    volume_program_bytes = 0;
    memset(volume_program, 0, n);
    CHECK(!find_program(root_start, &cluster, &size));
}

static void test_aap(void)
{
    static struct aap_reader r;
    struct aap_frame f;
    uint8_t req[16], stream[64];
    uint16_t major = 0, minor = 0, status = 1;
    int n, i;
    static const uint8_t want[] = { 0, 3, 0, 6, 0, 1, 0, 1, 0, 1 };
    n = aap_version_request(req);
    CHECK(n == 10 && memcmp(req, want, 10) == 0);
    /* A version response, then a frame of another channel, fed a byte
     * at a time as USB might cut them. */
    static const uint8_t resp[] = { 0, 3, 0, 8, 0, 2, 0, 1, 0, 6, 0, 0,  5, 3, 0, 2, 0xab, 0xcd };
    aap_reader_init(&r);
    n = 0;
    for (i = 0; i < (int)sizeof resp; i++) {
        CHECK(aap_feed(&r, resp + i, 1) == 0);
        while (aap_next(&r, &f)) {
            if (n == 0) {
                CHECK(i == 11);
                CHECK(aap_version_response(&f, &major, &minor, &status) == 0);
                CHECK(major == 1 && minor == 6 && status == 0);
            } else {
                CHECK(i == 17 && f.channel == 5 && f.length == 2 && f.payload[0] == 0xab && f.payload[1] == 0xcd);
                CHECK(aap_version_response(&f, &major, &minor, &status) == -1);
            }
            n++;
        }
    }
    CHECK(n == 2);
    /* A first frame of several has four bytes more of header. */
    static const uint8_t first[] = { 7, AAP_FIRST, 0, 2, 0, 0, 0x10, 0, 0x11, 0x22 };
    aap_reader_init(&r);
    CHECK(aap_feed(&r, first, sizeof first) == 0);
    CHECK(aap_next(&r, &f) == 1 && f.channel == 7 && f.length == 2 && f.payload[0] == 0x11);
    CHECK(aap_next(&r, &f) == 0);
    /* Too much without a frame's end. */
    aap_reader_init(&r);
    memset(stream, 0, sizeof stream);
    stream[2] = 0xff;
    stream[3] = 0xff;
    for (i = 0; i < (int)sizeof r.buf; i++)
        CHECK(aap_feed(&r, stream + (i < 4 ? i : 4), 1) == 0);
    CHECK(aap_next(&r, &f) == 0);
    CHECK(aap_feed(&r, stream, 1) == -1);
    /* The longest encrypted frame (16 KiB of video in an AES-GCM record:
     * 29 bytes more), a frame after it, in pieces as large as the
     * firmware feeds them, the frames taken out between pieces. */
    {
        static uint8_t big[8 + 16384 + 29 + 6];
        uint32_t len = 16384 + 29, at;
        int got = 0;
        big[0] = 3;
        big[1] = AAP_FIRST | AAP_ENCRYPTED;
        big[2] = (uint8_t)(len >> 8);
        big[3] = (uint8_t)len;
        big[4] = 0;
        big[5] = 1;
        for (at = 0; at < len; at++)
            big[8 + at] = (uint8_t)(at * 7);
        big[8 + len] = 3;
        big[8 + len + 1] = AAP_LAST | AAP_ENCRYPTED;
        big[8 + len + 3] = 2;
        aap_reader_init(&r);
        for (at = 0; at < sizeof big; at += AAP_FEED_MAX) {
            uint32_t k = sizeof big - at < AAP_FEED_MAX ? sizeof big - at : AAP_FEED_MAX;
            CHECK(aap_feed(&r, big + at, k) == 0);
            while (aap_next(&r, &f)) {
                if (got == 0)
                    CHECK(f.length == len && f.payload[0] == 0 && f.payload[len - 1] == (uint8_t)((len - 1) * 7));
                else
                    CHECK(f.length == 2 && (f.flags & AAP_LAST));
                got++;
            }
        }
        CHECK(got == 2);
    }
    /* The protocol buffer writer, read back by the reader. */
    {
        uint8_t buf[64], inner[16];
        struct aap_pb b, in;
        struct aap_pb_field fl;
        uint32_t at = 2;
        aap_pb_init(&in, inner, sizeof inner);
        aap_pb_varint(&in, 1, 800);
        aap_pb_init(&b, buf, sizeof buf);
        aap_pb_id(&b, AAP_SERVICE_DISCOVERY_RESPONSE);
        aap_pb_varint(&b, 1, 3);
        aap_pb_bytes(&b, 4, inner, in.len);
        aap_pb_string(&b, 2, "RNS-E");
        aap_pb_varint(&b, 13, 0x123456789ull);
        CHECK(!b.err && buf[0] == 0 && buf[1] == 6);
        CHECK(aap_pb_next(buf, b.len, &at, &fl) == 1 && fl.number == 1 && fl.type == 0 && fl.value == 3);
        CHECK(aap_pb_next(buf, b.len, &at, &fl) == 1 && fl.number == 4 && fl.type == 2 && fl.len == 3);
        CHECK(fl.bytes[0] == 0x08 && fl.bytes[1] == 0xa0 && fl.bytes[2] == 0x06);
        CHECK(aap_pb_next(buf, b.len, &at, &fl) == 1 && fl.number == 2 && fl.len == 5 && memcmp(fl.bytes, "RNS-E", 5) == 0);
        CHECK(aap_pb_next(buf, b.len, &at, &fl) == 1 && fl.number == 13 && fl.value == 0x123456789ull);
        CHECK(aap_pb_next(buf, b.len, &at, &fl) == 0);
        aap_pb_init(&b, buf, 4);
        aap_pb_string(&b, 2, "too long");
        CHECK(b.err && b.len == 4);
    }
}

int main(int argc, char **argv)
{
    card_reset(&card);
    /* sdbridge/tests/host IMAGE: the whole card into a file (for
     * tests/linkbench.rs). */
    if (argc == 2) {
        FILE *f = fopen(argv[1], "wb");
        uint32_t n, w[128];
        uint8_t b[512];
        int i;
        if (!f)
            return 2;
        for (n = 0; n < VOLUME_SECTORS; n++) {
            volume_sector(n, w);
            for (i = 0; i < 512; i++)
                b[i] = (uint8_t)(w[i / 4] >> (24 - 8 * (i % 4)));
            fwrite(b, 1, 512, f);
        }
        return fclose(f) ? 2 : 0;
    }
    test_crc();
    test_start();
    test_reads();
    test_damaged();
    test_block();
    test_volume();
    test_program_file();
    test_aap();
    printf(failures ? "sdbridge host tests: %d FAILED\n" : "sdbridge host tests: all passed\n", failures);
    return failures != 0;
}
