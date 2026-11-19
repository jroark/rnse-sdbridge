/* What aaview (sdk/examples/aaview) leaves for the viewer it starts
 * (sdk/examples/aaviewer), at a fixed address below it, as quakeboot
 * does for Quake (ports/docs/quake.md, "Memory"). */
#ifndef AA_HANDOFF_H
#define AA_HANDOFF_H

#define AA_HANDOFF 0x8c00f000u
#define AA_LOAD 0x8c010000u          /* aaviewer's LOAD (its image from +0x80): its file `place` */
#define AA_ROOM_END 0x8c7b0000u      /* LOAD + SIZE, below sdboot's session */

#define AA_MAGIC0 0x524e5345u        /* "RNSE" */
#define AA_MAGIC1 0x41415657u        /* "AAVW" */

struct aa_handoff {
    u32 magic[2];
    /* aaview's loader_return, for loader_return_via (0: none). */
    void (*loader_return)(int status);
    /* The slot of the card the viewer came from. */
    int slot;
};

#endif
