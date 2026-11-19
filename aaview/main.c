/* Android Auto's picture on the 192: the first stage, after
 * sdk/examples/sdboot.
 *
 * Copied to a card as AAVIEW.BIN (sdboot's menu lists it), beside
 * AAVIEWER.IMG (sdk/out/aaviewer-192.bin from sdk/build.sh 192
 * aaviewer; not .BIN, which sdboot would list, and which cannot run in
 * its place).  The viewer, with its H.264 decoder, is bigger than the
 * loader's place, so this program reads it to 0x8c010080, checks it,
 * leaves it what it needs at 0x8c00f000 (handoff.h) and enters it
 * (image_enter), as quakeboot does for Quake.
 *
 * It stays where it is, in the loader's place, and is the way back: the
 * viewer's loader_return goes through this program's (loader_return_via).
 * RETURN held for two seconds, here or in the viewer, does that.  The
 * display key and CD EJECT work all the while.
 *
 * Something missing or wrong is said on the screen, and after a few
 * seconds it is back to sdboot.  See docs/sdbridge.md, "The phone's
 * video into the slot". */
#include "rnse.h"
#include "handoff.h"

#define SESSION ((volatile u32 *)0xac7c0000)
#define SESSION_MAGIC 0x53444c31u

static int row;

static void say(const char *text, u8 colour)
{
    screen_fill(0, 70 + 18 * row, screen_width, 16, 0);
    screen_text(16, 70 + 18 * row, text, colour, 2);
    console_puts(text);
    console_puts("\n");
    row++;
}

static void wait_ms(u32 ms)
{
    u32 t = millis();
    while (millis() - t < ms) {
        panel_service();
        cpu_idle();
    }
}

static void give_up(const char *why, int r)
{
    char text[24], *p = text;
    u32 u = r < 0 ? (u32)-r : (u32)r;
    char t[12];
    int n = 0;
    say(why, 2);
    p = text;
    *p++ = 'E', *p++ = 'R', *p++ = 'R', *p++ = 'O', *p++ = 'R', *p++ = ' ';
    if (r < 0)
        *p++ = '-';
    do {
        t[n++] = (char)('0' + umod(u, 10));
        u = udiv(u, 10);
    } while (u);
    while (n)
        *p++ = t[--n];
    *p = 0;
    say(text, 1);
    say("BACK TO THE LOADER", 1);
    wait_ms(4000);
    loader_return(r ? r : -1);
    say("NO LOADER TO RETURN TO", 2);
    for (;;)
        wait_ms(1000);
}

int main(void)
{
    struct aa_handoff *h = (struct aa_handoff *)AA_HANDOFF;
    const u8 *image = (const u8 *)(AA_LOAD + 0x80);
    u32 size = 0, entry;
    int slot = -1, r = -2, i;

    board_init();
    console_puts("\nrnse sdk: aaview\n");
    screen_palette(1, 255, 255, 255);
    screen_palette(2, 200, 120, 40);
    screen_fill(0, 0, screen_width, screen_height, 0);
    screen_text(16, 16, "ANDROID AUTO", 2, 3);
    /* The faceplate controller as sdboot left it, lit. */
    panel_attach();
    sd_idle = panel_service;
    display_idle = panel_service;
    if (screen_width != 400)
        give_up("THE 192 ONLY", -1);

    /* The card sdboot read this program from; else the first there. */
    if (SESSION[0] == SESSION_MAGIC && SESSION[1] == ~SESSION_MAGIC && SESSION[3] < 2)
        slot = (int)SESSION[3];
    for (i = 0; i < 2 && r; i++) {
        int s = slot >= 0 ? slot : i;
        if (slot >= 0 && i)
            break;
        if (slot < 0 && !sd_present(s))
            continue;
        r = sd_attach(s);
        if (r)
            r = sd_init(s);
        if (!r)
            slot = s;
    }
    if (r)
        give_up("NO CARD", r);

    say("READING AAVIEWER.IMG", 1);
    r = sd_load("AAVIEWERIMG", (void *)image, AA_ROOM_END - 0x4000 - (u32)image, &size);
    if (r)
        give_up("AAVIEWER.IMG MISSING OR LONG", r);
    entry = ((const u32 *)image)[0];
    if (size < 0x1000 || ((const u32 *)image)[4] != AA_MAGIC0 || entry < (u32)image || entry >= (u32)image + size ||
        (entry & 1))
        give_up("AAVIEWER.IMG IS NOT THE VIEWER", -20);

    h->magic[0] = AA_MAGIC0;
    h->magic[1] = AA_MAGIC1;
    h->loader_return = loader_active() ? loader_return : 0;
    h->slot = slot;
    say("STARTING", 2);
    /* The viewer takes the motor over from a standstill. */
    if (display_state() == DISPLAY_OPENING || display_state() == DISPLAY_CLOSING)
        display_wait(9000);
    display_halt();
    image_enter(entry, AA_LOAD, AA_ROOM_END);
    return -31;
}
