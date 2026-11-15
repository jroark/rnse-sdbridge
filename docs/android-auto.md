# Android Auto on the 192: what has been found out

The question: can the 192 be an Android Auto head unit with its own
processor, display and sound, so that no more powerful computer is put
between the phone and the screen?  This file keeps what has been
measured apart from what is assumed, as `docs/custom-firmware.md` does.

## What Android Auto asks of a head unit, and what the 192 has

| Android Auto wants | The 192 has | So |
|---|---|---|
| A link to the phone: USB, with the head unit as host (Android Open Accessory), or wireless (5 GHz Wi-Fi, started over Bluetooth) | No USB, no Wi-Fi, no Bluetooth for data.  CAN, the drive's ATAPI bus, two SD slots behind their own processors, two unused serial channels (2 and 4) whose pins are not known | Something has to be added, and it has to carry bytes only (below) |
| H.264 video, constrained baseline, at 800x480 at least, 30 or 60 frames a second.  The head unit acknowledges frames and can hold the phone back by acknowledging slowly | SH7750R at 235 MHz, 16 KiB instruction and 32 KiB data cache, 64 MiB of RAM, no video hardware | The deciding question: see "Decoding" |
| A picture | Coral LQ (MB86293), a 400x240 panel (235 lines shown) | 800x480 is twice 400x240 each way: every other pixel of every other line, with its own chroma sample |
| Sound as PCM: media at 48 kHz stereo, guidance and system sounds at 16 kHz | Sound comes out of a card processor's converter at 44.1 kHz (22.5792 MHz crystal) and goes through the radio board's SAA7709H | A program of our own for a card processor, and 48 to 44.1 kHz on the main processor.  Not started |
| TLS 1.2, the head unit with a certificate signed by Google | The main processor | Its cost is to be measured.  The certificate can only come from a commercial head unit: like firmware, it is never committed |
| Input: a rotary controller and keys are enough (touch is optional) | The MMI knob and keys, through the faceplate controller | A good fit |
| Night mode, driving status, position (optional) | CAN, and the GPS on the board | Available |

### The link

An Eye-Fi card will not do: it is a closed card with 2.4 GHz Wi-Fi that
sends pictures from the card to a server (a service that ended in
2016), with no USB host and no Bluetooth, and to the unit it is only
storage.  No off-the-shelf board in the shape of an SD card has a USB
host either.  The plan is a small microcontroller (RP2040 or RP2350)
that is the USB host for the phone and answers as an SD card in one of
the slots, speaking a protocol of our own to a program of our own on
that slot's card processor.  It carries the phone's bytes and nothing
else: decoding, picture and sound stay on the unit.  On a bench it
would sit at the end of an SD extension cable; in a car it would be
soldered to one slot's contacts on the card board, which leaves the
other slot free.  None of this is built.

## Decoding

`sdk/examples/h264bench` decodes streams from an SD card with
`sdk/third_party/h264bsd` (the H.264 decoder that was in Android, in
portable C, baseline profile only; Apache 2.0 and MIT) and shows them
at half size, timing decoding and showing apart.  The streams come from
`tools/aa-streams.sh`: synthetic pictures, encoded as an Android Auto
phone encodes for a head unit that asks for 800x480 (constrained
baseline, CAVLC, one reference frame, no B frames, 30 frames a second),
five seconds each:

* MAP: a detailed picture panned slowly, as a moving map is (harder
  than most maps: more fine detail);
* LIST: a still screen with a highlight bar moving down a list (easy);
* FULL: motion over the whole picture (the worst case);
* MARGIN: MAP drawn into the top left 400x240 only, the rest black,
  which is what a phone sends when the head unit asks for 800x480 with
  margins that leave 400x240.  Whether phones honour such margins is
  not known yet.

Every decoded frame is checked: the program computes the CRC-32 of the
frames so far and compares it with what ffmpeg's decoder gives
(`NAME.CRC` on the card).  `tests/disc.rs`,
`sdk_decodes_h264_from_a_card`, does that in the emulator with a short
stream (`tests/data/margin.264`), started by the stock software
through `sdboot`, with the caches modelled.

### In the emulator

Every frame of all four streams decodes exactly as ffmpeg decodes it.
Decoding only, in frames a second (milliseconds a frame on average; the
slowest frame, a key frame, in brackets):

| stream | first build | `memset` a word at a time |
|---|---|---|
| FULL | 9.9 (100) | 13.7 (72; 108) |
| LIST | 21.5 (46) | 28.5 (35; 101) |
| MAP | 6.8 (146) | 9.7 (102; 231) |
| MARGIN | 8.6 (115) | 10.7 (92; 115) |

Showing a frame (the table look-up of `show`) costs some 7 ms more.

These count instructions: the emulator takes one instruction for one
cycle at 235 MHz and has no time for cache misses or the bus.  The
SH-4 can issue two instructions in a cycle, and main memory is slow
(95 ns an uncached store, `docs/hardware.md`), so the unit can come
out on either side of these figures.  **Only the unit can say**, and
the program is built to run there unchanged.  It runs every stream
twice, with its memory through P1 and through P0: the stock cache
setting the program finds (CCR 0x80008181) writes through in P1 and
copies back in P0, which the emulator does not time either.

The first build's profile (`--profile`, `tools/profile.py`) had a
third of the decoding time in the kit's `memset`, which went a byte at
a time, for the 2 KiB the decoder clears for each of a frame's 1500
macroblocks; that is what the second column fixed.  What remains is
spread out: interpolation for motion compensation about a third, then
`memcpy` (the decoder's copies of reference rows that cross the
picture's edge, and of coefficient counts, short and unaligned), the
loop filter and CAVLC.  Hand-written SH-4 code for the interpolation
and the loop filter is the obvious next step if the unit comes close.

### On the unit

The bench 192 (North American, software 0371), the program from a
32 MB card through `sdboot`, 2026-11-15, read off its screen.  Every
frame of every stream decoded as ffmpeg decodes it, on the real CPU as
in the emulator.  Frames a second decoding only, then with the picture
shown, then milliseconds a frame on average and the slowest (a key
frame each time):

| stream | memory through P1 (write-through) | through P0 (copy-back) |
|---|---|---|
| FULL | 5.3, 5.1; 186, 351 | 6.7, 6.3; 148, 246 |
| LIST | 10.4, 9.5; 95, 324 | 12.2, 11.0; 81, 221 |
| MAP | 3.9, 3.7; 255, 565 | 5.0, 4.7; 199, 404 |
| MARGIN | 4.9, 4.7; 202, 359 | 5.9, 5.6; 169, 249 |

The program found CCR 0x80008181 and MMUCR 0 (no address translation),
as the emulator has them.  Reading the eight files (twice over, 8272 KB)
from the card took 28.2 s: 293 KB/s, some 2.3 Mbit/s, through the card
processor and the stock software's program for it, a sector at a time.

So:

* The unit decodes at about half the emulator's rate (0.43 to 0.55 of
  it with copy-back): the emulator's one instruction a cycle is too
  kind for this kind of code, which waits for memory.  That ratio is
  worth remembering for any program heavy on memory.
* Copy-back is worth 20 to 28 % against the write-through the stock
  setting gives P1: decoding has to run with its frames in P0.
* With copy-back, typical 800x480 content decodes at 5 to 7 frames a
  second, and the margins case (5.9) is barely better than the whole
  picture of the same content (5.0): the black area is not free.

### The second round on the unit: the caches

The same unit and card, the same day, with `h264bench` 2: the streams
read into memory once, then decoded under five settings, every frame
checked (all 14 runs as ffmpeg decodes them), the results as CAN
records captured with a logic analyser.  Frames a second decoding only,
milliseconds a frame on average, the slowest frame, and the loop
filter's milliseconds a frame:

| setting | FULL | LIST | MAP | MARGIN |
|---|---|---|---|---|
| STOCK: CCR 0x80008181, the decoder as it came | 6.7; 150, 252; 28 | 12.5; 80, 225; 13 | 5.1; 196, 401; 24 | 6.0; 168, 251; 22 |
| COPY: full-pel blocks copied directly | 7.6; 131, 252; 28 | | 5.3; 189, 401; 24 | |
| COPY+CB: and P1 copy-back (0x80008185) | 13.8; 73, 124; 15 | | 8.9; 112, 232; 14 | |
| COPY+NOIX: and no index modes (0x80000101) | 8.2; 122, 238; 26 | | 5.7; 175, 378; 23 | |
| COPY+CB+NOIX (0x80000105) | **15.4**; 65, 112; 13 | **39.2**; 26, 100; 6 | **10.1**; 99, 211; 13 | **11.5**; 87, 113; 11 |

STOCK repeats the first round's copy-back figures (6.7, 12.2, 5.0,
5.9), so the measurement holds.

* **Stores through P1 written through were most of the unit's
  slowness.**  The decoder keeps its working buffers on the stack,
  which is in P1 like everything the kit's programs own, and the stock
  setting sends every store there to memory.  With P1 copy-back, FULL
  goes from 7.6 to 13.8 frames a second and MAP from 5.3 to 8.9.
* **The index modes cost a further tenth or so**: with IIX and OIX set,
  a program within one 32 MB has half of each cache.
* With both, the unit decodes at 92 to 98 % of the rate the emulator's
  instruction count gives (FULL 15.4 against 16.8, MAP 10.1 against
  10.3, MARGIN 11.5 against 11.2; LIST, almost all copies, 39 against
  53).  So the emulator's figures are now a fair guide, and fewer
  instructions are what makes it faster.
* Memory gives some 190 MB/s read and 89 MB/s copied (2 MB through P0,
  copy-back, under either setting).  The card gave 4136 KB in 13.9 s,
  297 KB/s.

`cache_setup` (`sdk/src/cache.S`) is how a program changes the setting
safely: it writes back and empties the operand cache through its
address array first, as the emulator's cache model now checks
(`--strict`).

### What would count as enough

At least 10 frames a second on typical 800x480 content: viable.  5 to
10: usable for navigation, with lag.  Under 5, even with margins: not
on the 192.

## Where that leaves it

With the caches set up for it, the 192 decodes typical 800x480 content
at 10 to 15 frames a second: at the "viable" line, before TLS, sound
and the link take their share of the processor.  Key frames take 110 to
210 ms, a hitch of a few frames each time one comes (phones send one
every few seconds or on demand).  What could still be gained, now that
instructions are what count: hand-written code for the interpolation
(a third of the instructions) and the loop filter (an eighth), and a
fast path for skipped macroblocks.

## Not yet done

If decoding gets fast enough: sound
through a card processor (PCM), TLS's cost, the Android Auto protocol
tried on a Mac against a phone (certificate, cipher, margins, the bit
rate when frames are acknowledged slowly), the link, and a short trial
of Linux in the emulator.
