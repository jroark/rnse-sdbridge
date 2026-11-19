/* The link from the bridge to the 192: the phone's video, as the 192
 * reads it through the card slot (SDLINK.UF2, sdbridge/src/sdlink.c, on one
 * side; aaviewer/ on the other; docs/sdbridge.md, "The phone's
 * video into the slot").
 *
 * The bridge's card has a file AALINK.BIN in its top directory (one run
 * of clusters).  Its first AALINK_CTL_SECTORS sectors say how things
 * stand; the AALINK_WINDOW sectors after them are a window onto a
 * stream of blocks numbered from 0 by `seq`: block seq is the window's
 * sector seq % AALINK_WINDOW.  Each block is 512 bytes, a header and
 * AALINK_PAYLOAD bytes of messages.  A message (the phone's codec
 * configuration, or a picture) starts a block of its own and fills
 * blocks to its end, its last block perhaps not full.
 *
 * The phone interleaves its channels, so the stream does too: every
 * block has the type of the message it carries, and a message's blocks
 * may have blocks of other types between them (video and sound: START
 * and END mark each message's first and last block of its own type).
 * The pictures are gathered whole; sound is played as it comes.
 *
 * The bridge keeps the blocks from `tail` to `head` (exclusive) in its
 * RAM.  A block below head reads as itself; one at or past head reads
 * as "not ready" (flags 0, with head as it is then): the 192 reads it
 * again later.  The 192 starting a read (CMD17 or CMD18) at block seq
 * says it has what came before: the bridge lets those go (tail = seq).
 * A block it reads is the same every time until then, so a read that
 * went wrong is simply made again.
 *
 * The phone sends its next picture when the bridge acknowledges the
 * last; the bridge does that once the picture's last block has gone out
 * to the 192, so the phone goes no faster than the 192 takes pictures.
 *
 * All numbers big-endian, in the order the bytes are on the card.
 *
 * Plain C, for the RP2040, the host's tests (sdbridge/tests/host.c) and
 * the 192's program alike.  It lives here, beside the 192's program,
 * because the SH-4 compiler's container sees only sdk/;
 * sdbridge/src/aalink.h includes it. */
#ifndef AALINK_H
#define AALINK_H

#define AALINK_NAME "AALINK  BIN"            /* as a directory entry has it */
#define AALINK_CTL_SECTORS 8
#define AALINK_WINDOW 65536u                 /* sectors: 32 MiB */
#define AALINK_INPUT_SECTORS 4096u           /* after the window: see "Input" below */
#define AALINK_INPUT_FIRST (AALINK_CTL_SECTORS + AALINK_WINDOW)
#define AALINK_FILE_SECTORS (AALINK_INPUT_FIRST + AALINK_INPUT_SECTORS)

/* The control sectors: 0 how things stand (below); 1 the same, and
 * reading it asks the phone for a new key frame (a 192 that starts while
 * the stream is under way, with the last key frame gone from the
 * bridge's RAM, needs one to start decoding from). */
#define AALINK_CTL_STATE 0
#define AALINK_CTL_KEY 1

/* A block: magic, seq, head (when the block was read), flags and the
 * bytes of payload used, a sum of the payload (the 32-bit words of all
 * AALINK_PAYLOAD bytes added, the unused ones zero), then the payload. */
#define AALINK_BLOCK_MAGIC 0x41414c42u       /* "AALB" */
#define AALINK_HEADER 20
#define AALINK_PAYLOAD (512 - AALINK_HEADER) /* 492 */
#define AALINK_READY 0x0001                  /* below head: a block of the stream */
#define AALINK_START 0x0002                  /* a message's first block */
#define AALINK_END 0x0004                    /* its last */
#define AALINK_KEY 0x0008                    /* (on START) a key frame: a picture to start decoding at */
#define AALINK_TYPE(f) (((f) >> 8) & 15)     /* what the block's message is */
#define AALINK_CONFIG 1                      /* the codec's configuration: H.264 parameter sets */
#define AALINK_PICTURE 2                     /* a picture: H.264 slices, start codes and all */
/* Sound, as the phone sends it (16-bit samples, little-endian, the
 * channels interleaved), its id and timestamp taken off: */
#define AALINK_SOUND_MEDIA 3                 /* music and the like: 48 kHz stereo */
#define AALINK_SOUND_SPEECH 4                /* navigation's voice: 16 kHz mono */
#define AALINK_SOUND_SYSTEM 5                /* the phone's own sounds: 16 kHz mono */
#define AALINK_IS_SOUND(t) ((t) >= AALINK_SOUND_MEDIA && (t) <= AALINK_SOUND_SYSTEM)

/* The state sector: magic, version, head, tail, the seq of the last key
 * frame's first block (AALINK_NONE: none since the phone started its
 * video), flags, the picture's size, how many of the codec
 * configuration's bytes follow at AALINK_CTL_CONFIG, and counts. */
#define AALINK_CTL_MAGIC 0x41414c43u         /* "AALC" */
#define AALINK_VERSION 1
#define AALINK_NONE 0xffffffffu
#define AALINK_PHONE 0x0001                  /* a phone in the session */
#define AALINK_VIDEO 0x0002                  /* its video under way */
#define AALINK_CTL_CONFIG 64
#define AALINK_CONFIG_MAX (512 - AALINK_CTL_CONFIG)

/* Input, the 192's keys and knob for the phone, and how far behind its
 * decoding is: reading sector AALINK_INPUT_FIRST + code (one CMD17) is
 * an event, whose code has a sequence number in bits 11-10 (the next of
 * each event, so that a read made again is not taken twice), the kind
 * in bits 9-8, and 8 bits of what:
 *
 *   key      (kind 0): an index of aalink_keys << 1, | 1 pressed
 *   knob     (kind 1): the rotary controller's detents, signed
 *   backlog  (kind 2): the pictures read and not yet decoded, said at
 *            every change and every half second; the bridge logs it.
 *            (Holding the phone's acknowledgements while it was 2 or
 *            more did not slow a Pixel down, and made its Android Auto
 *            leave the split screen: 2026-11-18.)
 *
 * The sector reads as the state sector.  Keys and the knob go to the
 * phone as Android Auto's input event; the codes are those the head unit
 * declares (src/aa_hu.c), as aasdk's ButtonCode has them. */
#define AALINK_IN_KNOB 0x100
#define AALINK_IN_BACKLOG 0x200
#define AALINK_IN_KIND(c) ((c) & 0x300)
#define AALINK_IN_CODE(seq, what) ((((seq) & 3u) << 10) | ((what) & 0x3ffu))
#define AALINK_KEY_HOME 0
#define AALINK_KEY_BACK 1
#define AALINK_KEY_UP 2
#define AALINK_KEY_DOWN 3
#define AALINK_KEY_LEFT 4
#define AALINK_KEY_RIGHT 5
#define AALINK_KEY_ENTER 6
#define AALINK_KEY_PLAY 7
#define AALINK_KEY_NEXT 8
#define AALINK_KEY_PREV 9
#define AALINK_KEYS 10
#define AALINK_KEY_CODES { 0x03, 0x04, 0x13, 0x14, 0x15, 0x16, 0x17, 0x55, 0x57, 0x58 }
#define AALINK_ROTARY 65536u                 /* the rotary controller's scan code */

/* Offsets in the state sector. */
#define AALINK_CTL_O_MAGIC 0
#define AALINK_CTL_O_VERSION 4
#define AALINK_CTL_O_HEAD 8
#define AALINK_CTL_O_TAIL 12
#define AALINK_CTL_O_KEY 16
#define AALINK_CTL_O_FLAGS 20                /* 16 bits */
#define AALINK_CTL_O_CONFIG_LEN 22           /* 16 bits */
#define AALINK_CTL_O_WIDTH 24                /* 16 bits */
#define AALINK_CTL_O_HEIGHT 26               /* 16 bits */
#define AALINK_CTL_O_MESSAGES 28             /* pictures the phone has sent */
#define AALINK_CTL_O_KEY_ASKED 32            /* key frames asked for (AALINK_CTL_KEY) */
#define AALINK_CTL_O_PAUSED 36               /* times the phone was held back for room */
#define AALINK_CTL_O_INPUTS 40               /* input events taken */

#endif
