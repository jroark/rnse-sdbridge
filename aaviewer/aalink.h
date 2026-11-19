/* The link from the bridge to the 192: the phone's video, as the 192
 * reads it through the card slot (SDLINK.UF2, sdbridge/src/sdlink.c, on one
 * side; sdk/examples/aaviewer on the other; docs/sdbridge.md, "The phone's
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
#define AALINK_FILE_SECTORS (AALINK_CTL_SECTORS + AALINK_WINDOW)

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
#define AALINK_TYPE(f) (((f) >> 8) & 15)     /* (on START) what the message is */
#define AALINK_CONFIG 1                      /* the codec's configuration: H.264 parameter sets */
#define AALINK_PICTURE 2                     /* a picture: H.264 slices, start codes and all */

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

#endif
