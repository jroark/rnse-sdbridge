# Android Auto on the 192 (and the 193): what has been found out

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
other slot free.  The first step of it is built, untried on a unit: an
RP2040 that answers as a read-only card for `linkbench`
([sdbridge.md](sdbridge.md)).  Its phone side has run: on a Raspberry
Pi Pico as USB host, a phone went into accessory mode and its Android
Auto answered the version request (2026-11-17).

### How fast a slot is: `linkbench`

The stock way into a slot, the installed software's card processor
program with a message for every sector, gave 297 KB/s on the unit
(below).  Android Auto's video at the 10 to 15 frames a second the 192
can decode, and its sound as PCM (1.5 Mbit/s for media), want some 3 to
4 Mbit/s.  `sdk/examples/linkbench` measures what a slot gives without
the stock program: it loads a program of its own into the slot's card
processor (written out by hand in its source, as `examples/report`'s
is) that runs every card command itself, the card's start included,
and moves sectors without a message each:

* CMD17, a command a sector, through the processor's buffer transfer
  unit;
* CMD18 in runs of 64 sectors, two buffers in turn, with a count of the
  blocks whose words had begun to arrive before their buffer was ready
  ("late": the host not keeping up);
* the host port alone, both ways: a count from the processor, words to
  it added up;
* on the main processor's side each word with the "nothing to read" pin
  looked at first (polled), or a block's first word so and the other 255
  read without looking (burst);
* the card interface's clock at the stock setting (the processor's
  clock / 4: 16.9 MHz at the assumed 67.7 MHz), / 3 (22.6 MHz) and / 2
  (33.9 MHz, over the 25 MHz of a card's default speed).

The card should have `LINKTEST.BIN` (`tools/linktest.py`, on a freshly
formatted card so that it lies in one run of clusters): every word of it
is a function of its place, and every word read is checked against it.
With the stock program (V222 0371) the program first copies the card
processor's port pins from it, since what pins 0 and 1 drive is not
known, and times the stock way over the same sectors.  Results on the
screen and on CAN 0x7F8 (records 0x5f-0x6a, in the source).  Nothing is
written to the card.  A burst that goes wrong leaves the processor out
of step with the host; the program notices, loads its program again,
starts the card again and goes on with the next test.

`tests/linkbench.rs` runs it in the emulator without firmware (no stock
program, so no pins and no stock time), every word checked:

| test | KB/s |
|---|---|
| host port, from the processor / to it | 12950 / 12050 |
| CMD17, / 4 | 1742 |
| CMD18, / 4, / 3, / 2 | 2091, 2786, 4170 |
| the same, burst | 2094, 2792, 4184 |

These say only that the protocol is no longer what limits: CMD18 runs
at the card interface's rate as the model has it, one data line (8
clocks a byte) at the divided clock, which a unit's interface may or may
not match.  The emulator gives a read of the host port one cycle of the
CPU (4 ns), so its host port figures mean nothing, and an unpaced burst
would outrun the card processor.  Since the run on the unit of
2026-11-17 the card processor polls its status before each word it
writes (a repeated move into the host port through a pointer lost
words there), about 120 ns a word, and a burst read is paced to match
(`BURST_PACE`, a pause of 20 turns of a loop before each read); what a
burst still saves is the status read before each word.

**On the unit, 2026-11-16, against the RP2040 bridge in the first
slot:** the stock way read 1 MB at 393 KB/s (3.1 Mbit/s), every word
right ([sdbridge.md](sdbridge.md), "Seen on hardware").  The program's
own tests did not run: its card processor program lost the words of
the card's answers on the unit (it wrote them by a repeated move into
the host port, which the emulator holds and a unit does not), so the
card's start stopped after CMD0.  It now polls the port's status before
each word (the run with the stock software is `tests/disc.rs`
`link_benchmark_from_the_sd_loader`), and on 2026-11-17 it ran through
against the bridge: **the host port carries 1.5 MB/s polled (12
Mbit/s) and 3.1 MB/s in bursts (25 Mbit/s), CMD17 809 KB/s, every word
right**; CMD18's blocks all came but their words were wrong, cause not
yet known ([sdbridge.md](sdbridge.md), "Seen on hardware").  Even
CMD17 alone carries more than the phone's whole stream measured on the
Pico (up to 660 KB/s).  To run it: `sdk/build.sh 192 linkbench`, the result
on a card as a `.BIN` for `sdboot`, with `LINKTEST.BIN` beside it (or
the bridge in the other slot).  What
to look for: CMD18's rate at each clock (the interface's real width and
clock), whether "late" stays at 0 when polled (whether a host of this
kind is fast enough for a stream), whether burst is clean, and errors in
the status column at / 2.  The main processor's DMA controller could
take the words instead (it does on the stock driver's reads), but the
request selector's setting for reads (at 0x12d00007 / 0x12d00003) is
not known: a trace of the stock driver (`--io-log` with a card) would
give it.

## Decoding

`sdk/examples/h264bench` decodes streams from an SD card with
`third_party/h264bsd` (the H.264 decoder that was in Android, in
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

(2026-11-18: the whole path is written: the bridge with the phone's
video in its card, `SDLINK.UF2`, and the viewer on the 192,
`sdk/examples/aaviewer`; tested in the emulator, and on the unit the
same evening it showed Google Maps from a phone at 10-13 pictures a
second, driven by the unit's knob and keys; no sound yet:
docs/sdbridge.md, "The phone's video into the slot".)

If decoding gets fast enough: sound
through a card processor (PCM), TLS's cost, the Android Auto protocol
tried on a Mac against a phone (certificate, cipher, margins, the bit
rate when frames are acknowledged slowly), `linkbench` on the unit and
then the bridge itself (the card side of the link, and writes for the
way back to the phone), and a short trial of Linux in the emulator.

## The 193

(2026-11-21.)  The same viewer, `aaviewer/`, runs on the 193 (8P0/8J0
035 193), started by rnse-sdk's `examples/sdload193` from a card in
slot 1 as `RNSE.BIN`, with the bridge (`SDLINK.UF2`) in slot 2.  On the
bench 193 it shows the phone's Android Auto (Google Maps, the media
card, the app bar) full screen, 800x480, at the phone's own 30 pictures
a second: some 17 ms decoding, 15 ms drawing and 3 ms reading a
picture, the bridge's 364 KB/s, no block read twice.  The 193's SH-4A
decodes about three times as fast as the 192.

What differs from the 192:

- **Reading.** The 193 reads the card through its own SD host (the
  kit's `sdhi_read_multi`, CMD18, 2.9 MB/s from the bridge on the
  unit), not through a card processor.  Its reads gulp the bridge's
  whole backlog each round, so the viewer decodes every waiting picture
  each round and draws the newest; decoding one a round (the 192's way,
  to keep the sound fed) never let the queue fall to one, and nothing
  was ever drawn.
- **The picture.** Shown whole, 800x480, not halved.  The kit draws on
  the 193 through a 16-bit plane (RGB555, as the stock software's
  planes); its earlier 8-bits-through-a-palette plane came out a flat
  light grey on the unit.
- **The board.** The bridge must be the Raspberry Pi Pico build at
  250 MHz (`SYS_KHZ=250000 ./build.sh`, `out/pico/SDLINK.UF2`); the
  RP2040-Zero build at 200 MHz answered the 193's CMD8 but not its
  CMD55.
- **The screen.** On the bench 193, whose software is locked by its
  anti-theft code, the screen stays dark until POWER is pressed, also
  while the viewer runs.
- **Sound.** The 193 plays the phone's sound through its own serial
  sound interface, not a card processor (the kit's `pcm_stream` on the
  193, from v0.4.0). The mix is the 192's, at 44.1 kHz, in blocks of
  2048 frames, twice the 192's length: the 193 takes no interrupt for
  sound, so the ring moves on only when the viewer pumps it. The
  amplifier is set over the 193's radio processor line. Tested in the
  emulator (`viewer_plays_the_phone_s_sound_on_the_193`); not yet heard
  on the unit.
- **The steering wheel.** As on the 192 (the kit's car driver).
  Since kit v0.4.0 the driver takes the CAN controller as the stock
  software left it (`can_attach`). Tested in the emulator
  (`viewer_follows_the_steering_wheel_on_the_193`); not yet on the unit.
- **Keys.** The knob and keys go to the phone as on the 192, and CD
  EJECT ejects; the display key opens and closes the display (kit v0.4.0,
  its motor lines read off the stock software; tested in the emulator,
  `viewer_keys_on_the_193`, not yet on a unit); RETURN held says
  there is no loader to return to (the 193's loader is on the disc).
- **Times.** Until kit v0.4.0 the kit's clock ran twice too fast on a
  real 193 (its tick handler was entered twice for each tick: rnse's
  `docs/hardware-193.md`, "Timer interrupts").  A viewer built on an
  earlier kit kept all its times at half the world's: its time-outs,
  its pacing of the sound, and the milliseconds in its reports, which
  are half of what a picture really took.  The figures measured on the
  unit before that have not been taken again.
- **Reports.** The bench has no console, so the viewer reports on CAN
  0x7F8 (records 0xd0-0xd7, in `aaviewer/main.c`; 0xd6 the sound,
  0xd7 the car).

Needs rnse-sdk v0.4.0 or later (sound and CAN on the 193; v0.3.0 for the 193's SD host, its 16-bit
picture, `build.sh` building an example with a `place` file for the
193).
