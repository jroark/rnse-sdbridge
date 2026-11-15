/* How fast the 192 decodes the video an Android Auto phone sends: an
 * H.264 decoder (third_party/h264bsd, baseline profile, in C) run over
 * the streams of tools/aa-streams.sh, 800x480 at 30 frames a second,
 * from an SD card (examples/sdboot starts it as RNSE.BIN).
 *
 * The files NAME.264 of the card's top directory (up to four) are read
 * into memory once.  Then they are decoded under several settings of
 * the caches and the decoder (the table `configs` below), every frame
 * timed with the tick's timer, the loop filter's share apart, and shown
 * on the screen at half size (400x240, of which the screen has 235
 * lines) in 8 bits a pixel through a palette of 3-3-2 bits.  The CRC-32
 * of the frames so far goes to the debug port after each; that of them
 * all is checked against the last line of NAME.CRC on the card, which
 * tools/aa-streams.sh writes from ffmpeg's decoder.  The decoder's
 * memory, and the streams, are in RAM through P0.
 *
 * Results on the screen and as records on CAN 0x7F8, as examples/suite
 * and examples/timing send them: test, item, a value of 32 bits and
 * one of 16.
 *
 *   0x2f  the run: CCR as the program found it (0), the memory to
 *         allocate from (1), MMUCR (2), milliseconds reading the files
 *         from the card and their kilobytes (3)
 *   0x2e  memory, in kilobytes a second through P0: reading 2 MB with
 *         the caches as found (0) and with CB and no index modes (1);
 *         copying 2 MB likewise (2, 3)
 *   0x30 + 0x10 * c + n: setting c, the n-th stream:
 *         0  frames decoded, frames with errors
 *         1  microseconds decoding, all frames
 *         2  microseconds of the slowest frame, its number
 *         3  microseconds decoding the frames that start a sequence
 *            (IDR), how many there were
 *         4  microseconds showing, all frames
 *         5  CRC-32 of all frames, 1 = equal to the card's NAME.CRC
 *            (0 different, 2 no NAME.CRC)
 *         6  microseconds in the loop filter, all frames
 *         7  CCR during the run, the decoder's switches
 *         8  frames a second decoding (times ten), and decoding and
 *            showing (times ten)
 *   0xff  done; a summary of all on the screen for a minute, then
 *         back to the SD loader (examples/sdboot) by returning.
 *
 * The keys are answered all the while (panel_service, after every
 * piece of the stream): the display key and CD EJECT work, and RETURN
 * held for two seconds puts the caches back and returns to the SD
 * loader.
 *
 * The 192 only. */
#include "rnse.h"
#include "h264bsd_decoder.h"

#define CCR 0xff00001c
#define MMUCR 0xff000010
#define VRAM 0xb8000000
#define SURFACE 0x00110000
#define STRIDE 448

/* Memory for the streams and the decoder: RAM below the stock loader's
 * place (0x8dd00000), above the way back's scratch (way_back_scratch
 * and 4 MB), through P0.  What the unit's software had there is gone
 * by now. */
#define HEAP 0x0c800000
#define HEAP_END 0x0dcf0000
/* The program's tables, above the heap: they do not fit in the
 * program's own 128 KiB with the decoder. */
#define TABLES 0x8dcf0000

#define WHITE 0xff
#define BLACK 0x00

static int row;
static u32 *const crc_table = (u32 *)TABLES;                 /* 1 KiB */
static u8 *const clamp_table = (u8 *)(TABLES + 0x400);       /* 1 KiB */
#define CLAMP(v) clamp_table[(v) + 384]
/* Colour by 5 bits of luma and 4 each of the two chroma samples:
 * plenty for 3-3-2 bits of colour, and one look-up a pixel. */
#define COLOURS (32 * 16 * 16)
static u8 *const colour = (u8 *)(TABLES + 0x800);            /* 8 KiB */
#define PIXEL(y, cb, cr) colour[((y) >> 3) << 8 | ((cb) >> 4) << 4 | (cr) >> 4]

static char *put_dec(char *p, u32 v)
{
    char t[10];
    int n = 0;
    do {
        t[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n)
        *p++ = t[--n];
    return p;
}

static char *put_str(char *p, const char *s)
{
    while (*s)
        *p++ = *s++;
    return p;
}

/* A tenth as "12.3". */
static char *put_tenths(char *p, u32 v)
{
    p = put_dec(p, v / 10);
    *p++ = '.';
    *p++ = (char)('0' + v % 10);
    return p;
}

static void line(const char *text)
{
    int y = 2 + 10 * row;
    screen_fill(0, y - 1, screen_width, 10, BLACK);
    screen_text(2, y, text, WHITE, 1);
    console_puts(text);
    console_puts("\n");
    if (++row >= 23)
        row = 0;
}

static void rec(u8 test, u8 item, u32 value, u32 second)
{
    u8 d[8] = { test, item, (u8)(value >> 24), (u8)(value >> 16), (u8)(value >> 8), (u8)value, (u8)(second >> 8), (u8)second };
    can_send_for(0x7f8, d, 8, 3000);
}

static void tables(void)
{
    u32 i, j, c;
    s32 k;
    for (i = 0; i < 256; i++) {
        for (c = i, j = 0; j < 8; j++)
            c = c & 1 ? 0xedb88320 ^ (c >> 1) : c >> 1;
        crc_table[i] = c;
    }
    for (k = -384; k < 640; k++)
        clamp_table[k + 384] = (u8)(k < 0 ? 0 : k > 255 ? 255 : k);
    /* ITU-R BT.601, studio range, in 1/256, for the middle of each
     * step of the table. */
    for (i = 0; i < COLOURS; i++) {
        s32 y = (s32)(i >> 8) * 8 + 4, cb = (s32)((i >> 4) & 15) * 16 + 8, cr = (s32)(i & 15) * 16 + 8;
        s32 l = (y - 16) * 298 >> 8;
        u32 r = CLAMP(l + ((409 * (cr - 128)) >> 8));
        u32 g = CLAMP(l - ((208 * (cr - 128)) >> 8) - ((100 * (cb - 128)) >> 8));
        u32 b = CLAMP(l + ((516 * (cb - 128)) >> 8));
        colour[i] = (u8)((r & 0xe0) | (g & 0xe0) >> 3 | b >> 6);
    }
    /* The palette: 3 bits red, 3 green, 2 blue. */
    for (i = 0; i < 256; i++)
        screen_palette((int)i, (u8)((i >> 5) * 255 / 7), (u8)(((i >> 2) & 7) * 255 / 7), (u8)((i & 3) * 255 / 3));
}

static u32 crc32(u32 crc, const u8 *p, u32 n)
{
    crc = ~crc;
    while (n--)
        crc = crc_table[(crc ^ *p++) & 0xff] ^ (crc >> 8);
    return ~crc;
}

/* Every other pixel of every other line, with its own chroma sample
 * (the chroma planes are already at half size), four pixels to a
 * long word written to video memory: pixel N of the surface is the
 * byte at N ^ 3, so the first of four is the lowest byte. */
static void show(const u8 *pic, u32 w, u32 h)
{
    const u8 *cb = pic + w * h, *cr = cb + w * h / 4;
    u32 x, y, lines = (u32)screen_height < h / 2 ? (u32)screen_height : h / 2, cols = w / 2 > 400 ? 400 : w / 2;
    for (y = 0; y < lines; y++) {
        const u8 *py = pic + 2 * y * w, *pb = cb + y * (w / 2), *pr = cr + y * (w / 2);
        volatile u32 *out = (volatile u32 *)(VRAM + SURFACE + y * STRIDE);
        for (x = 0; x < cols; x += 4) {
            *out++ = (u32)PIXEL(py[0], pb[0], pr[0]) | (u32)PIXEL(py[2], pb[1], pr[1]) << 8 |
                     (u32)PIXEL(py[4], pb[2], pr[2]) << 16 | (u32)PIXEL(py[6], pb[3], pr[3]) << 24;
            py += 8;
            pb += 4;
            pr += 4;
        }
    }
}

/* The last line of a .CRC file: 8 hex digits.  1 when found. */
static int expected_crc(const char *name264, u32 *crc)
{
    static char text[2048];
    char name[12];
    u32 size, i, v = 0;
    int digits = 0;
    for (i = 0; i < 8; i++)
        name[i] = name264[i];
    name[8] = 'C', name[9] = 'R', name[10] = 'C', name[11] = 0;
    if (sd_load(name, text, sizeof text - 512, &size) || size < 9)
        return 0;
    /* Back from the end over the line break, then eight digits. */
    i = size;
    while (i > 0 && (text[i - 1] == '\n' || text[i - 1] == '\r'))
        i--;
    if (i < 8)
        return 0;
    for (i -= 8; digits < 8; i++, digits++) {
        char c = text[i];
        v = v << 4 | (u32)(c >= 'a' ? c - 'a' + 10 : c >= 'A' ? c - 'A' + 10 : c - '0');
    }
    *crc = v;
    return 1;
}

/* The settings tried, in this order.  `all`: every stream, else MAP
 * and FULL only (the two that matter most, to keep a round short). */
static const struct {
    const char *name;
    u32 ccr, fast_copy;
    int all;
} configs[] = {
    /* As the stock software leaves the caches, the decoder as it came. */
    { "STOCK", 0x80008181, 0, 1 },
    /* Full-pel blocks copied directly (third_party/h264bsd/README.md). */
    { "COPY", 0x80008181, 1, 0 },
    /* And stores through P1 (the stack, the program's own data) copied
     * back instead of written through. */
    { "COPY+CB", 0x80008181 | CCR_CB, 1, 0 },
    /* The caches indexed by the plain address (IIX, OIX off): all of
     * each for code and data within one 32 MB. */
    { "COPY+NOIX", CCR_EMODE | CCR_ICE | CCR_OCE, 1, 0 },
    /* Both. */
    { "COPY+CB+NOIX", CCR_EMODE | CCR_ICE | CCR_OCE | CCR_CB, 1, 1 },
};
#define CONFIGS (int)(sizeof configs / sizeof configs[0])

static struct {
    char name[9];
    u8 *data;
    u32 size, want;
    int check;
} streams[4];
static int nstreams;

/* What the summary at the end shows of each run. */
static struct {
    int config, stream;
    u32 fps, ms, slowest, filter_ms, crc;
} result[32];
static int results;

static u32 load_ms, load_kb;

static char *put_hex(char *p, u32 v)
{
    int i;
    for (i = 28; i >= 0; i -= 4)
        *p++ = "0123456789ABCDEF"[(v >> i) & 15];
    return p;
}

static void bench(int c, int n, u8 *heap)
{
    u8 test = (u8)(0x30 + 0x10 * c + n);
    u32 t, len, read, frames = 0, bad = 0, dec = 0, since = 0, slowest = 0, slowest_at = 0;
    u32 idr_us = 0, idrs = 0, show_us = 0, crc = 0, w = 0, h = 0, fps, fps_all;
    u8 *p;
    storage_t *d;
    char text[64], *q;

    heap_init(heap, (u32)((u8 *)HEAP_END - heap));
    q = put_str(text, configs[c].name);
    q = put_str(q, ": ");
    q = put_str(q, streams[n].name);
    *q = 0;
    line(text);
    h264bsdFastCopy = configs[c].fast_copy;
    h264bsdFilterTime = 0;
    /* The decoder takes the emulation prevention bytes out of the
     * stream where it is: it decodes a copy, so that the stream can be
     * decoded again. */
    p = malloc(streams[n].size);
    d = h264bsdAlloc();
    if (!p || !d || h264bsdInit(d, 1) != 0) {
        line("  NO MEMORY FOR THE DECODER");
        return;
    }
    memcpy(p, streams[n].data, streams[n].size);
    len = streams[n].size;
    while (len > 0) {
        u32 r, id, idr, errs;
        u8 *pic;
        t = micros();
        r = h264bsdDecode(d, p, len, 0, &read);
        t = micros() - t;
        dec += t;
        since += t;
        /* The keys, between two pieces of the stream (outside the
         * timing): the display key and CD EJECT, and RETURN held. */
        panel_service();
        /* (After the parameter sets the decoder says so and takes
         * nothing: the same bytes again then.) */
        if (read > len || (read == 0 && r != H264BSD_HDRS_RDY))
            break;
        p += read;
        len -= read;
        if (r == H264BSD_MEMALLOC_ERROR) {
            line("  OUT OF MEMORY");
            break;
        }
        if (r == H264BSD_ERROR || r == H264BSD_PARAM_SET_ERROR)
            bad++;
        if (r != H264BSD_PIC_RDY)
            continue;
        w = h264bsdPicWidth(d) * 16;
        h = h264bsdPicHeight(d) * 16;
        while ((pic = h264bsdNextOutputPicture(d, &id, &idr, &errs)) != 0) {
            /* The CRC-32 of the frames so far, as each line of the
             * .CRC file has it. */
            crc = crc32(crc, pic, w * h * 3 / 2);
            console_hex(crc);
            console_puts(idr ? " idr " : " ");
            console_dec(since);
            console_puts("\n");
            if (since > slowest)
                slowest = since, slowest_at = frames;
            if (idr)
                idr_us += since, idrs++;
            if (errs)
                bad++;
            frames++;
            since = 0;
            t = micros();
            show(pic, w, h);
            show_us += micros() - t;
        }
    }
    h264bsdShutdown(d);

    /* Frames a second, times ten: frames * 10000 / milliseconds. */
    fps = dec / 1000 ? frames * 10000 / (dec / 1000) : 0;
    fps_all = (dec + show_us) / 1000 ? frames * 10000 / ((dec + show_us) / 1000) : 0;
    rec(test, 0, frames, bad);
    rec(test, 1, dec, 0);
    rec(test, 2, slowest, slowest_at);
    rec(test, 3, idr_us, idrs);
    rec(test, 4, show_us, 0);
    rec(test, 5, crc, streams[n].check ? crc == streams[n].want : 2);
    rec(test, 6, h264bsdFilterTime, 0);
    rec(test, 7, configs[c].ccr, configs[c].fast_copy);
    rec(test, 8, fps, fps_all);

    q = put_str(text, "  ");
    q = put_dec(q, frames);
    q = put_str(q, " FRAMES ");
    q = put_dec(q, w);
    q = put_str(q, "X");
    q = put_dec(q, h);
    q = put_str(q, ", DECODE ");
    q = put_tenths(q, fps);
    q = put_str(q, " FPS");
    *q = 0;
    line(text);

    if (results < (int)(sizeof result / sizeof result[0])) {
        result[results].config = c;
        result[results].stream = n;
        result[results].fps = fps;
        result[results].ms = frames ? dec / frames / 1000 : 0;
        result[results].slowest = slowest / 1000;
        result[results].filter_ms = frames ? h264bsdFilterTime / frames / 1000 : 0;
        result[results].crc = streams[n].check ? (frames - bad > 0 && crc == streams[n].want) : 2;
        results++;
    }
}

/* What memory gives, in kilobytes a second, through P0 under the cache
 * setting of the moment: 2 MB read a long word at a time, and 2 MB
 * copied (memcpy), each from memory the caches do not hold. */
static u32 mem_read_kbs[2], mem_copy_kbs[2];

static void memory_speed(int k, u8 *at)
{
    volatile const u32 *p = (volatile const u32 *)at;
    u32 t, i, sum = 0;
    t = micros();
    for (i = 0; i < 0x200000 / 4; i += 8)
        sum += p[i] + p[i + 1] + p[i + 2] + p[i + 3] + p[i + 4] + p[i + 5] + p[i + 6] + p[i + 7];
    t = micros() - t;
    mem_read_kbs[k] = t ? 2048000000u / t : 0;
    t = micros();
    memcpy(at + 0x400000, at + 0x200000, 0x200000);
    t = micros() - t;
    mem_copy_kbs[k] = t ? 2048000000u / t : 0;
    rec(0x2e, (u8)k, mem_read_kbs[k], sum & 0xffff);
    rec(0x2e, (u8)(k + 2), mem_copy_kbs[k], 0);
}

/* The streams into memory, one after the other from the start of the
 * heap; returns where the free memory starts. */
static u8 *load(void)
{
    char name[12];
    u32 size, t0 = millis();
    int i;
    u8 *at = (u8 *)HEAP;
    for (i = 0; nstreams < 4 && sd_list(i, name, &size) == 0; i++) {
        int k;
        char text[48], *q;
        /* (A Mac leaves a file "._NAME" beside each it writes.) */
        if (name[8] != '2' || name[9] != '6' || name[10] != '4' || name[0] == '_' || size < 1024)
            continue;
        if (at + size + 512 > (u8 *)HEAP_END - 0x01000000)
            break;
        q = put_str(text, "READING ");
        for (k = 0; k < 8 && name[k] != ' '; k++)
            *q++ = streams[nstreams].name[k] = name[k];
        streams[nstreams].name[k] = 0;
        q = put_str(q, ".264, ");
        q = put_dec(q, size >> 10);
        q = put_str(q, " KB");
        *q = 0;
        line(text);
        if (sd_load(name, at, size + 512, &size)) {
            line("  CANNOT READ IT");
            continue;
        }
        streams[nstreams].data = at;
        streams[nstreams].size = size;
        streams[nstreams].check = expected_crc(name, &streams[nstreams].want);
        load_kb += size >> 10;
        at += (size + 512 + 31) & ~31u;
        nstreams++;
    }
    load_ms = millis() - t0;
    return at;
}

static int wanted(int c, int n)
{
    const char *s = streams[n].name;
    return configs[c].all || (s[0] == 'M' && s[1] == 'A' && s[2] == 'P' && !s[3]) ||
           (s[0] == 'F' && s[1] == 'U' && s[2] == 'L' && s[3] == 'L' && !s[4]);
}

/* Everything on one screen, to be read off it (or photographed). */
static void summary(u32 ccr, u32 mmucr)
{
    char text[64], *q;
    int i, pad;
    screen_fill(0, 0, screen_width, screen_height, BLACK);
    row = 0;
    q = put_str(text, "H264BENCH 2  CCR ");
    q = put_hex(q, ccr);
    q = put_str(q, "  MMUCR ");
    q = put_hex(q, mmucr);
    *q = 0;
    line(text);
    line("SETTING       STREAM   FPS    MS  MAX  FILT  CRC");
    for (i = 0; i < results; i++) {
        const char *cn = configs[result[i].config].name, *sn = streams[result[i].stream].name;
        q = put_str(text, cn);
        for (pad = 0; cn[pad]; pad++)
            ;
        for (; pad < 14; pad++)
            *q++ = ' ';
        q = put_str(q, sn);
        for (pad = 0; sn[pad]; pad++)
            ;
        for (; pad < 8; pad++)
            *q++ = ' ';
        q = put_str(q, " ");
        q = put_tenths(q, result[i].fps);
        q = put_str(q, "  ");
        q = put_dec(q, result[i].ms);
        q = put_str(q, "  ");
        q = put_dec(q, result[i].slowest);
        q = put_str(q, "  ");
        q = put_dec(q, result[i].filter_ms);
        q = put_str(q, result[i].crc == 1 ? "  OK" : result[i].crc == 0 ? "  BAD" : "  -");
        *q = 0;
        line(text);
    }
    line("FPS: DECODING ONLY.  MS: A FRAME ON AVERAGE.");
    line("MAX: THE SLOWEST.  FILT: LOOP FILTER, MS A FRAME.");
    q = put_str(text, "MEMORY KB/S, STOCK: READ ");
    q = put_dec(q, mem_read_kbs[0]);
    q = put_str(q, " COPY ");
    q = put_dec(q, mem_copy_kbs[0]);
    *q = 0;
    line(text);
    q = put_str(text, "  CB+NOIX: READ ");
    q = put_dec(q, mem_read_kbs[1]);
    q = put_str(q, " COPY ");
    q = put_dec(q, mem_copy_kbs[1]);
    *q = 0;
    line(text);
    q = put_str(text, "CARD READ: ");
    q = put_dec(q, load_kb);
    q = put_str(q, " KB IN ");
    q = put_dec(q, load_ms);
    q = put_str(q, " MS");
    *q = 0;
    line(text);
}

/* RETURN held: the caches back as the program found them, before the
 * loader takes over. */
static u32 found_ccr;

static void leave(void)
{
    cache_setup(found_ccr);
}

int main(void)
{
    u32 ccr, mmucr;
    int slot, c, n;
    u8 *heap;

    board_init();
    console_puts("\nrnse sdk: h264bench\n");
    if (screen_width != 400) {
        console_puts("the 192 only\n");
        return 1;
    }
    tables();
    screen_fill(0, 0, screen_width, screen_height, BLACK);
    can_init();
    ccr = REG32(CCR);
    mmucr = REG32(MMUCR);
    found_ccr = ccr;
    panel_init();
    sd_idle = panel_service;
    display_idle = panel_service;
    loader_leave = leave;
    rec(0x2f, 0, ccr, 0);
    rec(0x2f, 1, HEAP_END - HEAP, 0);
    rec(0x2f, 2, mmucr, 0);
    line("RNSE SDK: H264BENCH 2.  RESULTS ON CAN 7F8");
    line("HOLD RETURN 2 S: BACK TO THE LOADER");
    h264bsdTimer = micros;

    for (slot = 0; slot < 2; slot++)
        if (sd_attach(slot) == 0 || sd_init(slot) == 0)
            break;
    if (slot == 2) {
        line("NO CARD");
    } else if (mmucr & 1) {
        /* The decoder's memory is used through P0, which would be
         * translated. */
        line("ADDRESSES ARE TRANSLATED (MMUCR.AT): NOT RUN");
    } else {
        heap = load();
        rec(0x2f, 3, load_ms, load_kb);
        memory_speed(0, heap);
        cache_setup(CCR_EMODE | CCR_ICE | CCR_OCE | CCR_CB);
        memory_speed(1, heap + 0x600000);
        cache_setup(ccr);
        if (nstreams == 0)
            line("NO .264 FILES ON THE CARD");
        for (c = 0; c < CONFIGS && nstreams; c++) {
            cache_setup(configs[c].ccr);
            for (n = 0; n < nstreams; n++)
                if (wanted(c, n))
                    bench(c, n, heap);
        }
        cache_setup(ccr);
    }
    rec(0xff, 0xff, millis(), 0);
    if (results)
        summary(ccr, mmucr);
    line("");
    line("DONE.  BACK TO THE SD LOADER IN 60 S");
    {
        u32 t0 = millis();
        while (millis() - t0 < 60000) {
            panel_service();
            if (panel_return_held())
                line("NO LOADER TO RETURN TO");
            cpu_idle();
        }
    }
    /* To the loader that started the program (examples/sdboot), which
     * waits for the next card.  (Started otherwise, the program stops
     * here, in the kit's idle loop.) */
    return 0;
}
