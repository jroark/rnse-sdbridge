# The SD bridge: an RP2040 as a card in a 192's slot

The plan for Android Auto on the 192 (`docs/android-auto.md`) puts a
microcontroller in one of the unit's SD slots: the USB host for the
phone on one side, an SD card on the other, carrying bytes only.  This
is the first step of it, on a Waveshare RP2040-Zero: the board answers
as an SD card and nothing else.  The card it is reads only, and is made
up as it is read: 64 MiB, a partition table, FAT16, and `LINKTEST.BIN`
(8 MiB, the words of `tools/linktest.py`) in its top directory.  That is
what `sdk/examples/linkbench` reads and checks, so the benchmark times
the bridge as it would a card, and says whether every word arrived.

Not yet tried on a unit.  What has been tested without one is listed at
the end, and so is what only a unit can say.

To build and carry on from your own computer: [sdbridge-laptop.md](sdbridge-laptop.md).

## Wiring

| SD card contact | Signal | RP2040-Zero |
|---|---|---|
| 5 | CLK | GP2, through 100 ohms |
| 2 | CMD | GP3, through 100 ohms |
| 7 | DAT0 | GP4, through 100 ohms |
| 8, 9, 1 | DAT1, DAT2, DAT3 | not connected (GP5-GP7 kept for four lines later) |
| 3, 6 | VSS | GND |
| 4 | VDD | **not connected** |

The contacts are those of the card end of the extension: on a card, 1
is DAT3 at the notched corner, then 2 CMD, 3 VSS, 4 VDD, 5 CLK, 6 VSS,
7 DAT0, 8 DAT1, 9 DAT2.  GP2-GP4 are on the board's edge (as its print
says).

* **The board's power is its own**, from its USB connector (a computer,
  for the log, or a charger), never the slot's 3.3 V: two supplies tied
  together fight.  Only the ground is shared.
* **Power the board before the unit, and keep it powered while it is
  plugged in.**  An RP2040 without power takes current through its pins
  from a host that drives them; the resistors limit that, and they also
  limit the current while both sides drive a line during bring-up.  At
  16.9 MHz and with a short ribbon, 100 ohms against the line's few
  picofarads costs nothing (about 1.5 ns).
* Keep the ribbon short and a ground wire beside CLK.
* The slot's own switch must see the card end go in: the unit knows a
  card by it (`docs/hardware.md`, "SD card processors"), not by any
  contact.

**The display and the disc.**  The slots are behind the fold-down
display.  While the ribbon is in, leave the display open and lay the
cable so that it cannot catch in the display's motor or the drive's
opening: do not close the display on it.  The CD drive's eject and the
display key keep working in `linkbench`, as in every program of the
SDK.

### On a Raspberry Pi Pico

The same firmware runs on an original Raspberry Pi Pico, from
`sdbridge/out/pico/`.  Its LED is a plain one, so the states the
RP2040-Zero shows in colour are patterns there (`src/status.h`): a short
flash every 2 s waiting, blinking twice a second busy, flickering at
work, steady on for OK, two or three short flashes every 2 s for a
warning or a failure.  The card's wiring on a Pico:

| Signal | Pico pin |
|---|---|
| CLK | 4 (GP2), through 100 ohms |
| CMD | 5 (GP3), through 100 ohms |
| DAT0 | 6 (GP4), through 100 ohms |
| GND | 3 or 8 |

Pin 1 is the corner next to the micro-USB socket on the side marked
"1"; the pins count away from the socket on that side (1-20) and back
on the other (21-40, so 40 is the other corner by the socket).

## Building and flashing

```sh
sdbridge/build.sh            # sdbridge/out/SDBRIDGE.UF2
```

It clones the pico-sdk 2.1.1 and its TinyUSB (pinned by commit, into
`sdbridge/work/`, never committed) and needs git, cmake, a host C
compiler and `arm-none-eabi-gcc` with newlib (Debian and Ubuntu:
`gcc-arm-none-eabi libnewlib-arm-none-eabi`; macOS: the Arm GNU
toolchain).  `SYS_KHZ=250000 sdbridge/build.sh` for a faster processor
(200 MHz by default).

To flash: hold the board's BOOT button while plugging it into a
computer, and copy `SDBRIDGE.UF2` onto the drive `RPI-RP2` that appears.
The board restarts with it.  Nothing about the unit is flashed: the
bridge offers no way to.

## What it shows

The board's LED, once a second: **blue** the card not started, **green**
started (selected or stand-by), **white** read from in the last second,
**red** a damaged command (bad CRC7) in the last second.

Its USB connector is also a serial port (any baud rate), with the
commands of the card's start as they come (`CMD8 000001aa`, `ACMD41
40ff0000`, ...; reads are only counted) and a line a second:

```
state tran  commands 412  reads 130  blocks 8320 (2088 KB/s)  bad CRC 0  refused 0  slowest answer 610 cycles (52 of the host's clocks at 16.9 MHz)
```

"Slowest answer" is the longest the command loop took to have a
response ready, from the command's last bit.  A card has 64 of the
host's clocks for that, and the response goes out two clocks after it
is ready at the least, so anything under 60 at the clock in use is in
time.  It is the figure to watch at / 3 and / 2.

## Running the benchmark against it

1. Flash the board, wire it as above, power it, put the card end into
   the slot `sdboot` calls SLOT 1.  `linkbench` tests the first slot
   with a card in it.
2. Put a card with `sdboot`'s programs and `LINKBENC.BIN`
   (`sdk/build.sh 192 linkbench`) into the other slot, and start it from
   `sdboot`'s menu.
3. With the stock software (V222 0371) the benchmark first starts the
   bridge through the stock card processor program and times the stock
   way, then takes the processor over and starts the bridge again with
   its own program; both starts show in the bridge's log.

The two figures to compare with the same benchmark on a real card: the
rates (the card interface's, if the bridge keeps up) and the bad sector
counts, which should be 0 everywhere.

### Iterating with the bridge on the laptop's USB

The bridge can carry the program too, so that a new build of
`linkbench` needs no card and no photograph.  It has `LINKBENC.BIN` on
its card (after `LINKTEST.BIN`, which is too big for `sdboot` to take
for a program, so `LINKBENC.BIN` is its only one and starts by itself),
held in RAM and sent over the board's USB serial port:

```sh
python3 sdbridge/link.py serve /dev/cu.usbmodem* sdbridge/work/bridge.log sdbridge/work/push/LINKBENC.BIN &
cp sdk/out/linkbench-192.bin sdbridge/work/push/LINKBENC.BIN   # each new build
python3 sdbridge/link.py results sdbridge/work/bridge.log       # what the last run said
```

`serve` logs the bridge and sends the file whenever it changes
(`PUT <bytes> <FNV-1a>` and the bytes; the file is gone from the
bridge's card until all of it is there).  On the unit the bridge goes in
SLOT 2 and the card to test in SLOT 1 (or the bridge alone, to test
itself).  `linkbench` tests SLOT 1's card, and when the other slot's
card has `LINKBENC.BIN` it sends everything through that card by the
stock program there, its screen's text included (`results` prints it),
and then waits: every 2 s it reads `LINKBENC.BIN` again, and when that
has changed it runs it in its own place (`run_image`).  So after the
first start every new build runs by itself; RETURN held still goes back
to the loader.  `link.py bootsel PORT` restarts the board into its
`RPI-RP2` drive (the port at 1200 baud) for a new UF2, with no button.
`tests/disc.rs` `link_benchmark_from_the_sd_loader` runs the loop on the
emulated unit, a changed card standing in for a push.

## How it works

* `src/sd.pio`: two PIO programs.  On PIO0 the command line: a command
  of 48 bits in, read at the clock's rising edges; the CPU's answer; the
  response out, starting two clocks after the command at the earliest.
  On PIO1 DAT0: a block's start bit, 4096 bits, CRC16 and end bit, from
  words the DMA controller feeds it.  Both change their outputs just
  after a rising edge (15 ns at 200 MHz), so a bit is steady across the
  falling edge and up to the next rising one; the host's hold time is
  5 ns.  The LED's WS2812 is on PIO1 too.
* `src/card.c`: the card's states and commands (start-up, CMD9/10's
  registers, CMD13, 16, 17, 18, 12; writes, protection and erasure
  refused as write-protected, ACMD6 for one line only, anything else
  refused as illegal), standard capacity, byte addresses.  No SDK in
  it, so that it is tested on the host.
* `src/volume.c`: the made-up card, a sector at a time.
* `src/main.c`: core 1 only answers commands (in RAM, as everything:
  the image is copied there at start, so that no flash read holds up a
  response); core 0 makes the sectors asked for, two buffers in turn so
  that the next is ready while one goes out, and keeps the LED and the
  log.  A new read or CMD12 stops the data state machine where it is.

## What has been tested, and what only a unit can say

Tested here (`sh sdbridge/tests/run.sh`, and `cargo test --release --test
linkbench`):

* the card's answers to the start-up as `sdk/src/sd.c` and `linkbench`
  send it, the registers (the CSD says 64 MiB), the reads, the refusals,
  a damaged command (`tests/host.c`);
* the CRCs against the SD specification's examples, and CRC16 against a
  bit-by-bit one;
* the volume as a FAT reader reads it: partition, boot sector, the two
  FATs alike, the file's chain in one run, its words;
* the PIO programs, as pioasm made them, in a model of the state
  machines with the input synchronisers, against a host's waveforms at
  264 kHz, 16.9, 22.6 and 33.9 MHz, the CPU's answer prompt and late,
  every phase of the clock: the bits of a response and of two blocks,
  the spacing (Ncr, and between blocks), the lines let go after
  (`tests/piosim.py`; it found two timing faults of the first version);
* the volume, written to an image, through `linkbench` on the emulated
  192: the SDK's FAT code finds the file, and every word checks.

Not tested: the firmware on a board (it builds, without warnings), the
SDK's PIO and DMA set-up, USB, the LED's colours, and anything
electrical.  Only a unit can say:

* which edge the NEC card interface reads on, and how long it gives a
  card to answer (the bridge assumes the SD specification's);
* whether 64 clocks are enough at / 3 and / 2 (the log's "slowest
  answer");
* whether the interface's clock really is 16.9 MHz at the stock
  setting.

**Seen on hardware, 2026-11-16:** a Raspberry Pi Pico with `SDBRIDGE.UF2`
at 200 MHz, in the first slot of the user's 192 (V222, software 0350)
on a ribbon, CLK, CMD and DAT0 through 100 ohm, ground, no VDD (the Pico
powered from USB), and `linkbench` started by the SD loader from a card
in the second slot.  The installed software's card processor program
started the bridge as a card, found `LINKTEST.BIN` on its volume and
read 1 MB of it at **393 KB/s (3.1 Mbit/s) with every word right**, no
block missing or late: the bridge works as a card for the stock driver,
at the stock clock.  `linkbench`'s own tests did not run: its program
for the card processor, loaded in place of the stock one, did not get
past the card's start.  A second run on 2026-11-17 with the Pico's log
said where: the stock start (CMD0, CMD8, ACMD41 rounds, CMD2, 3, 9, 10,
7, 16, a step every 150 ms, the slowest answer 70 of the host's clocks)
and 2068 reads at 383 KB/s, then the benchmark's program sent CMD0 some
ten times, once for each start, about 200 ms apart, and never a CMD8:
the main processor got the header of each answer and not its words.
The program wrote them by a repeated move into the host port, which
the emulator holds until the host reads and a unit evidently does not
(docs/hardware.md, "SD card processors"); it now polls the status
before each word.

The third run (2026-11-17, the same set-up, `LINKBENC.BIN` sha256
`68b5f50e`) went all the way through, with the bridge's log:

| Test | KB/s | Mbit/s | Bad sectors (of 4096) | Not in time | Late | Status |
|---|---|---|---|---|---|---|
| stock driver (CMD17, a message a sector) | 393 | 3.1 | 0 | 0 | 0 | |
| port out (card processor to host, polled) | 1547 | 12.3 | 0 | 0 | 0 | |
| port in (host to card processor, polled) | 1594 | 12.7 | 0 | 0 | 0 | |
| CMD17 / 4 | 809 | 6.4 | 0 | 0 | 0 | |
| CMD18 / 4 | 1525 | 12.2 | 4094 | 0 | 3968 | 8020 |
| CMD18 / 3 | 1529 | 12.2 | 4096 | 0 | 3983 | 8020 |
| port out, burst | 3128 | 25.0 | 0 | 0 | 0 | |
| CMD18 / 4, burst | 2075 | 16.6 | 4092 | 0 | 0 | 8020 |
| CMD18 / 3, burst | 2753 | 22.0 | 4027 | 0 | 1136 | 8020 |
| CMD18 / 2 | 1533 | 12.2 | 4096 | 0 | 4005 | 8060 |
| CMD18 / 2, burst | 3070 | 24.5 | 4096 | 0 | 4010 | 8020 |

So: the bridge answers every command and every block comes (no
timeouts; on its side no bad CRC, and its slowest answer 70 of the
host's clocks at the stock 16.9 MHz), CMD17 from it is right to the
word at 809 KB/s, twice the stock way, and the host port carries 1.5
MB/s polled and 3.1 MB/s in bursts.  Polled, the host is slower than
the card at / 4 (2.1 MB/s), so CMD18's blocks come before the program
has a buffer for them ("late"); but CMD18's words are wrong even where
none was late (/ 4 burst), so something in the multi-block read itself
differs on the unit, the bridge's or the program's: the card
interface's status has bit 5 set (and bit 6 at / 2), which the emulator
does not give a meaning.  `linkbench` now looks at the first bad sector
of each test (a line `T<test> S<sector> W<word> <came>/<wanted> D<where
in the file the word that came is> K<bits shifted> R<bad first sectors
of a command>`), and a run with an ordinary card in the bridge's place
tells the bridge from the program.

The fourth run (the same, `LINKBENC.BIN` sha256 `d4657f9d`) gave the
same figures and said what the bad words were: `T5 S0 W0
1B06640C/640CA17E` and the like at the start of every command, the
words one 16-bit word late behind a stray one, which the card
interface had kept from the block the bridge had begun when the last
CMD18 was stopped.  The emulator now keeps one the way the unit does
(docs/hardware.md, "SD card processors").  The fifth run (sha256
`5b412254`) read the data register before each command while SDCTL bit
0 was set, which takes the word in the emulator, and changed nothing
on the unit (`T7 S0 W0 81DA640C/640CA17E` again, the same stray word as
before), so on the unit the word is somewhere that bit does not
report.  The sixth run (sha256 `5ada9dbe`) tried four ways of
getting rid of it after its tests ("WHAT TAKES THE STRAY WORD"): only a
one-word buffer transfer did (`M2 C0C07/0C06 640CA17E RIGHT`; nothing,
a read of the data register and the interface's reset left
`0CF7640C`), and the program now does that before every read (sha256
`96bb4402`).  The seventh run with it: `M0` and `M2` right, `M1` and
`M3` wrong, as the emulator has it; CMD18 / 4 in bursts 2074 KB/s with
no block late and 256 of 4096 sectors wrong, / 3 in bursts 2753 KB/s,
64 late, 256 wrong; polled, every command's first two blocks in time
and the rest late (3968 of 4096).  Which 256, and how, is the next
question (the program since, sha256 `cc673adb`, reports the bursts'
bad sectors first, with where in a command they fall).  The eighth run
answered part of it: in bursts (/ 4: 320 of 4096 wrong, none late)
only a sector's **last word** is wrong (`T7 S512-3647 W127
5DB9CD3D/5DB979D3`, `T8 S768-3647 W127 698B88B7/698BF087`), in sectors
spread over the middle of the commands (5 first, 5 last of one); the
wrong word is neither a neighbouring sector's word nor the block's
CRC16.  The program since (sha256 `eef6f412`) counts the blocks whose
last word a burst found not yet in the port (`X` on the `T` lines) and
reads / 5 in bursts with the usual pause and with twice it.  The
ninth run: / 5 in bursts right (1664 KB/s), with twice the pause 704
wrong, and never a last word found missing: it is not the host
reading too soon.  From the tenth run on `linkbench` sends everything
as the arguments of CMD16s that the bridge logs (its "results through
the card"), the first bad sector of each test whole among them: in
bursts the bad sectors are whole commands of 64, in each of them every
sector's last 16-bit word wrong and the rest right, in both buffers
alike (moving the second from 0x5100 to 0x5200 changed nothing), the
wrong word the same for a sector from run to run and neither the
block's CRC16 nor any word near it.  The program since (sha256
`2904084f`) reads the card processor's own copy of a bad command's
last two words, to tell the way in from the way out.  The
eleventh run: the card processor's copy is as wrong as the host's (16
commands of 16), so the word goes wrong on the way in, between the
bridge's DAT0 and the card processor's buffer; the same command at / 4
and at / 3 got the same wrong words, so it is a function of the data
and not of the moment.  The same with the 32 MB card in the first
slot instead of the bridge: the stock way read it (352 KB/s, every
word right), but `linkbench`'s own card start stopped at CMD2 twice
(`OWN PROGRAM: STEP 7`), where the bridge goes through; so that card
told nothing about CMD18 yet.  The start since (sha256 `83e6047f`)
stores the interface's setting again before CMD2 as the stock driver
does, tries a second time at the stock driver's 150 ms a step, and
shows what each try got (CMD8's and CMD2's headers, ACMD41's rounds
and OCR).  With it the 32 MB card still stopped at CMD2, and its OCR
read `8000FF3F` where `80FF8000` was due.  Waiting about 1 ms before
reading the answer (sha256 `62dc9f5b`) changed nothing, and ACMD41's
raw words told why: `FF80 00FF 3F00`, round and round over the eight
reads.  The response register gives the three words of a short answer
over and over, from where the last read left off, also into the next
answer; the program read eight of every one, so each answer started 8
mod 3 = 2 words further on.  ACMD41's first answer happened to be read
in place and said busy (`3F00 FF80 00FF`, OCR `00FF8000`); its second
was read a word late, and the `80` that took the place of the busy bit
made it ready.  CMD2 went to a card that had not finished its start.
The bridge is ready at once, so its first answer was enough, and its
CMD3 answer came out in place after the setting stored before CMD2
(so that setting, or CMD2's long answer, puts the read position back:
which of the two is not known).  The program since (sha256
`6da038de`) reads 3 words of a short answer and 8 of a long one, as the
stock program does; the emulator's response register goes round the
same way, and with an old card that is busy for a few rounds the
eight-read program gives exactly the unit's screen.  With
it (2026-11-18) the card stopped one step earlier, at ACMD41 (`STEP 5`):
its first round's answer was CMD55's (`3700 4001 204F`) and its second
none.  The program had polled SDCTL bit 2 for the answer, which is
still up from the last response for a moment after a command starts,
and so took CMD55's answer for ACMD41's and started the next command
over one still on the line.  The program since (sha256 `0b41ae3b`)
waits for the response's interrupt as the stock program does: CMD55's
came and ACMD41's did not (`A41 0`), because the interrupt comes when
bit 2 goes up and it was still up from CMD55's.  The stock program
starts each command by writing bits 0-2 back as it read them, and a 1
takes bit 2 down (docs/hardware.md); the program since (sha256
`def57680`) does that too.  From here on with the bridge carrying the program and the
results (below, "Iterating with the bridge on the laptop's USB"), five
builds in half an hour: ACMD41's answer raises no interrupt at all, and
the wait now also ends on an error flag; CMD9 got no answer and is
passed over; and with that **linkbench's own program started the 32 MB
card and ran every test on it** (sha256 `74fd8fec`).  The card's
multiple-block reads in bursts (`CMD18 /4 BURST`, `/5 BUR40`) had the
bridge's fault exactly: the sector's last 16-bit word wrong and the
rest right (`EE01C927` where `EE018223` was due), here in nearly every
sector, not in whole commands; so it is not the bridge's doing but the
card interface's, or the program's handling of it.  The polled ones lost
words as with the bridge (late blocks), and single-block reads were
right but every block late (51 KB/s, `T4095`), which the bridge never
was.  A map of each test's first 128 sectors and of the headers
of their blocks (later builds) showed where: **the first command of 64
blocks came right every time, and every later one went wrong from its
second block on**.  Its first block came in right but its transfer did
not say it was done (the program waited for it 35 ms, `T`), the card
went on sending, and from then on the program took blocks tens of
sectors further on, from the middle of one (sector 65's place had
sector 101's 241st 16-bit word on).  So it is the restart after CMD12,
not the reading: **one CMD18 for the whole 2 MB came in without a
wrong word, at 2036 KB/s** (`CMD18 /4 B ONE`, 2026-11-18).  Asking the
card's status (CMD13) before each later command mended most of it (64
bad sectors of 4096, one a command, with the last word wrong, and a
block a command not done in time), a wait of 1 ms did nothing, and the
card interface's reset left the stopped read's word in front of the
next (as `docs/hardware.md` has it).  The last word that comes out as
the block's CRC shifted is the same thing seen from the other side:
the last word of a block after a restart is stored late.  For a link
the answer is long reads: a CMD18 that runs as long as the stream does.
Most of that was one thing, found the same afternoon: the response
interrupt comes only when SDCTL bit 2 goes up, and nothing linkbench
did took it down between commands (docs/hardware.md).  Reading the
response register before each command until it is down (sha256
`d3ed8fe0`): single-block reads 653 KB/s, 64-block CMD18s in bursts
2007 KB/s with no wrong word at / 4, where they had been 51 KB/s and
nearly every sector wrong.  What is left: now and then a whole command
(1-7 of 64 at / 3 to / 5) whose every block has its last word stored
late, the first symptom seen with the bridge; the state that decides
it is set when the command begins and does not show in SDCTL.  That
went too, the same evening: after each CMD12 the card interface reset
and the stopped read's word taken by a one-word transfer
(docs/hardware.md).  Since (sha256 `c2c65b59`), from the 32 MB card in
SLOT 1: single-block reads 653 KB/s, CMD18 bursts of 64 blocks at / 3
2626 KB/s, / 4 2007 KB/s, / 5 1618 KB/s, all without a wrong word, and
one CMD18 for the whole 2 MB 2055 KB/s.  What is not clean is what is
too fast for the host: polled CMD18s (1.5 MB/s off the port) and / 2
in bursts.
What stays a matter of speed: a polled host
takes 1.5 MB/s off the port, less than the card gives at / 4 (2.1
MB/s), so polled CMD18 blocks come before the program has a buffer for
them ("late") and are lost; in bursts (3.1 MB/s) / 4 had no late block
and / 3 (2.8 MB/s) few.

## The phone on the desk: `SDPHONE.UF2`

The second step needs no unit: the board as the USB host of your phone,
doing what an Android Auto head unit does first.  It asks the phone to
switch into accessory mode (Android Open Accessory), and when the phone
comes back in that mode, sends Android Auto's version request and waits
for the answer.  That shows the cable, the power, the phone's accessory
mode and its Android Auto, before anything else depends on them.  No
certificate and no TLS yet: the version exchange comes before both.

`sdbridge/build.sh` builds it beside the card (`SDPHONE.UF2`); flash it
the same way.  It is a separate firmware because the board's USB port is
the host here, so its log moves to a serial line: **GP0** (UART0 TX,
115200 baud), for a USB serial adapter or a second Pico running
debugprobe.  The log is optional: the LED says how far it got.

| LED | Meaning |
|---|---|
| blue | waiting for a phone |
| yellow | a phone: asking it to switch to accessory mode |
| cyan | in accessory mode, the version request sent |
| **green** | Android Auto answered and the versions match: this step works |
| magenta | it answered, the versions do not match (the log has them) |
| red | the device has no accessory mode, or a transfer failed; unplug to retry |

**Cabling.**  The phone must be the USB *device*.  A USB-C to USB-C cable
lets the phone decide it is the host, and it will.  So use the board's
USB-C port with a USB-C (male) to USB-A (female) OTG adapter, and the
phone's ordinary USB-A to USB-C cable into that.  The A-to-C cable's
pull-up tells the phone that the other end is the host.

**On a Raspberry Pi Pico:** a micro-USB (male) to USB-A (female) OTG
adapter in the Pico's socket, the phone's USB-A to USB-C cable into
that, and 5 V into **pin 40 (VBUS)** with ground on **pin 38**.  VBUS is
the USB socket's own 5 V and reaches the board through its diode to
VSYS, so this one feed powers both the Pico and the phone's side of the
cable (as the Pico's datasheet has it for a USB host).  The log is on
pin 1 (GP0, TX), ground on pin 3.  Flash first (hold BOOTSEL while
plugging the Pico into a computer, copy `out/pico/SDPHONE.UF2`), then
unplug the computer before connecting the 5 V: a computer on the
micro-USB socket and a supply on pin 40 would be two supplies on one
wire.

**Power.**  The board's own USB port is busy, so power it through its
**5V** pin, from a supply that can also charge a phone (a phone draws up
to an amp or more from a host): a USB power bank or a 5 V adapter on a
breakout.  The phone only offers its USB side when it sees 5 V on the
cable, so check with a meter, before plugging the phone in, that the
5 V on the board's 5V pin also reaches VBUS on the adapter's USB-A
socket.  If the board passes it through a diode that blocks this way,
feed 5 V to the socket's VBUS directly.  Never connect a second supply
to the board's USB-C port while the 5V pin is powered.

**On the phone.**  Android Auto installed (it is part of the system on
recent Android) and, in Android Auto's settings, *Add new cars to
Android Auto* on: otherwise it ignores a head unit it has not seen.  The
phone may ask whether to allow the "Android Auto" accessory: allow it.

What the log looks like when it works:

```
sdphone: waiting for a phone on the USB port
device 1: 18d1:4ee1
accessory protocol version 2
asked to start accessory mode: the phone should come back as 18d1:2d00
device 1 gone
device 1: 18d1:2d01
accessory endpoints: in 81, out 01
version request sent
frame: channel 0, flags 03, 8 bytes
Android Auto answered: version 1.x, they match
```

(The phone's own ids on the second line, and the version on the last,
depend on the phone.)

The framing (`src/aap.c`) follows aasdk, the open-source head unit
library: Google does not publish the protocol.  It is tested on the host
(`tests/host.c`): the request's bytes, a response cut anywhere, two
frames in one transfer, the longer header of a first frame of several.

**Seen on hardware, 2026-11-17:** on a Raspberry Pi Pico (`out/pico/`),
powered through VBUS (pin 40) and cabled through a micro-USB OTG
adapter, the user's phone switched to accessory mode, said "Android Auto
connected to your vehicle", and answered the version request with
matching versions (the LED steady on).  So the cabling, the power, the
RP2040 as USB host, the accessory strings and the framing of the first
message all hold with a real phone.  What follows the version exchange,
TLS, is not done yet, and the phone gives up on the session after it.

## The TLS handshake, with a test certificate: `SDHANDSHAKE.UF2`

After the version exchange, Android Auto runs a TLS 1.2 handshake inside
its own messages (message 0x0003 on channel 0), the head unit as the
client, presenting a certificate of its own.  Then the head unit sends
"authentication complete" (message 0x0004), and everything after it is
encrypted.  The third desk test runs that part on the Pico, with the
pico-sdk's mbedTLS (2.28): `src/aa_tls.c` moves mbedTLS's bytes in and
out of Android Auto's frames, `src/sdhandshake.c` is the firmware.

**With a test certificate it cannot succeed.**  The phone accepts only a
head unit certificate issued under Google's Android Auto programme, and
the repository has none and will not have one.  A self-signed stand-in
lets everything up to that check run: the handshake's messages in
Android Auto's frames, mbedTLS on the RP2040 (its memory, and how long
each step takes on a Cortex-M0+), and the phone's answer, which is a TLS
alert; the session ends there.

**With a certificate the phone accepts** (user-supplied, kept outside
the repository like the stand-in and never committed) the handshake
completes, and the firmware goes on to "authentication complete" and
the phone's service discovery request.  `SDSTREAM.UF2` (below) carries
on from there.

Some head unit certificates give their dates with a time zone (a
UTCTime such as `140704000000-0700`).  RFC 5280 does not allow that and
mbedTLS refuses to parse them (`MBEDTLS_ERR_X509_INVALID_DATE`); phones
accept them.  `pem2c.py` notices this and puts two forms in the header:
a copy with the dates in UTC and the lengths around them mended, for
mbedTLS to parse (its signature no longer matches, but the head unit
does not verify its own certificate), and the certificate as it was.
`aa_tls_init` replaces the parsed certificate's raw bytes with the
original, so the phone gets the original bytes.

### Making the stand-in

Anywhere outside the repository, with OpenSSL (or LibreSSL, as macOS
has it):

```sh
mkdir -p ~/rnse-test-cert && cd ~/rnse-test-cert
openssl req -x509 -newkey rsa:2048 -nodes -days 365 \
    -subj "/CN=rnse test head unit" \
    -keyout test-hu.key -out test-hu.crt
```

A 2048-bit RSA key and a certificate signed by itself, both in PEM.
`.gitignore` keeps `*.pem`, `*.key`, `*.crt` and `*.der` out of the
repository all the same; do not put them in it under other names.

### Building with it

```sh
AA_CERT=~/rnse-test-cert/test-hu.crt AA_KEY=~/rnse-test-cert/test-hu.key sdbridge/build.sh
```

`sdbridge/pem2c.py` turns the two files into `sdbridge/work/hu_cert.h`
(ignored by git) and the build adds `out/SDHANDSHAKE.UF2` and
`out/pico/SDHANDSHAKE.UF2`.  Without `AA_CERT` and `AA_KEY` it leaves
that firmware out (and removes an old header and UF2).  The header and
the UF2s hold the key: treat them like the key file.

Flash and cable as for `SDPHONE.UF2` (5 V on VBUS, the OTG adapter).
TinyUSB and TLS run on core 1 with a 16 KiB stack; the static buffers
leave mbedTLS some 95 KiB of heap.

### What it shows

The log (GP0, 115200 baud):

```
version request sent
Android Auto answered: version 1.x, they match
handshake: 0 bytes from the phone, ... ms on the Pico
handshake: ... bytes to the phone            (ClientHello)
handshake: ... bytes from the phone, ... ms on the Pico
handshake: ... bytes to the phone            (the certificate, the key exchange, ...)
the phone ended the handshake with alert 42 (bad certificate)
with a test certificate, that is the expected end of this test
```

The milliseconds are the RP2040's time for each step: the key exchange
and the signature with the head unit's key are the slow ones on a
Cortex-M0+, and whether the phone waits for them is one of the things
this shows.  The alert number depends on the phone (42 bad certificate,
48 unknown CA, 46 certificate unknown are the likely ones).

The LED: WAIT, then BUSY through the switch and the handshake, and FAIL
(three short flashes every 2 s) after the alert, which with a test
certificate is the expected end.  With a certificate the phone accepts,
it would go on: WORKING once the handshake is done and authentication
complete sent, OK when the phone's service discovery request has been
decrypted, its text fields (the phone's name among them) in the log.

### Tested here

`sh sdbridge/tests/run.sh` builds `tests/tls_host.c` on the host with
the same mbedTLS and runs `aa_tls.c` as the head unit against an
mbedTLS server standing in for the phone, every byte through Android
Auto's frames, cut into 64-byte pieces as USB delivers them.  Its
certificates are made by OpenSSL for the test only, in
`sdbridge/work/tls-test/`:

* a phone that trusts the head unit's certificate: the handshake
  completes (ECDHE-RSA with AES-256-GCM), authentication complete goes
  across, an encrypted service discovery request comes out of the
  frames and is read, and an encrypted answer goes back;
* a phone that trusts only another authority, as a real phone trusts
  only Google's: it ends the handshake with alert 48 (unknown CA), and
  the head unit reports it.

Not tested: the firmware on a board, how long the steps take there, and
a real phone's answer.

## The phone's video: `SDSTREAM.UF2`

The fourth desk test is the handshake firmware built with
`AA_STREAM=1` (`src/sdhandshake.c`), carrying on as a head unit
(`src/aa_hu.c`):

* it answers the service discovery request with three channels, as
  aasdk numbers them: input (1, an 800x480 touch screen and a few keys),
  sensors (2, driving status and night mode) and video (3, 800x480 at
  30 frames a second, 140 dpi).  It offers no sound channel, so the phone
  keeps its sound;
* it opens the channels the phone asks for and refuses the others;
* it answers a sensor start with the first value (unrestricted, day), and
  input binding, pings, audio and navigation focus as aasdk's head unit
  does;
* it sets up the video channel, with `AA_MAX_UNACKED` frames allowed
  ahead of the acknowledgements (1 by default, as aasdk has it;
  `AA_MAX_UNACKED=4 sdbridge/build.sh` for more), and gives it focus;
* it acknowledges each video message once its last frame is in.  The
  messages are counted, never assembled: a key frame can be larger than
  the RP2040 has room for.

It reads the USB 16 KiB at a time.  The buffers are sized for frames of
16 KiB of plain text in a TLS record, which is what the phone sends.
The log (GP0, 115200 baud) has the handshake as above and then:

```
the phone's service discovery request: name "..." brand "..."
service discovery response: ... bytes, channels input 1, sensors 2, video 3 (800x480)
channel 2 open (priority 0): ok
sensor start: type 13 (driving status)
channel 3 open (priority 0): ok
video setup request (configuration 0): ok, 1 frames ahead of the acknowledgements
video start: session ..., configuration 0
video frames arriving, ... ms after authentication
codec configuration (... bytes): 00 00 00 01 67 ...
H.264 profile 66 (Baseline), constraints .., level 3.1
  12s: video 30 fps, 150 KB/s, 0 key; USB 152 KB/s, 60 frames; decrypting 20% of the time; ...
summary: ... video frames (... key frames) in ... s, ... KB, ...
```

The numbers in that example are made up, not measured.  Each second's
line gives the phone's video rate through the RP2040's full-speed USB
(frames, KB/s, key frames), the raw USB rate, and the share of the
Pico's time spent decrypting.  That share is what it would cost the
RP2040 to hand the 192 plain text.  The profile line matters for the
192's decoder, which takes Baseline only.  The LED is green once video
arrives.

**Seen on hardware, 2026-11-16** (a Raspberry Pi Pico, a Pixel 11 Pro XL
with Android Auto's protocol 1.7, the log on GP0 and the phone's own
through adb over Wi-Fi):

* The handshake: the phone accepted the head unit certificate and agreed
  ECDHE-RSA with AES-128-GCM.  The Pico's slow step, the key exchange
  and the signature with the head unit's key, took 4.0 s, and the phone
  waited for it.
* With only input, sensors and video in the service discovery response,
  the phone ended the session at once ("startRequiredServices failed",
  right after its audio set-up): a phone of 2026 wants the sound
  channels and a microphone.  With them (`aa_hu.c` now offers all seven),
  every channel opened, the phone showed its "Welcome to Android Auto"
  prompt, and after it the session was connected and Maps projected on
  an 800x480 screen at 140 dpi.
* The stream is **H.264 Baseline, level 3.1** (constraint flags 0x40),
  which is what the 192's decoder (h264bsd) takes.  Key frames were up
  to 77 KB.
* The video configuration's frame rate value 1, which aasdk calls 30
  frames a second, gave up to 58 frames a second: current phones read
  1 as 60 and 2 as 30.
* **The link was the limit.**  TinyUSB's RP2040 host ran the
  accessory's bulk endpoints on the controller's polled "interrupt"
  endpoints, one 64-byte packet per 1 ms frame: 56 KB/s in all, the
  phone's sound at 459 kbps of the 1536 it needs (147 ms an
  acknowledgement), and the video stopped.
  `patches/tinyusb-rp2040-host-bulk-epx.patch` (applied by `build.sh`)
  runs bulk transfers on EPX instead, a packet a transaction, started
  again from the interrupt.  With it the USB carried a steady **310 to
  320 KB/s**: the phone's sound at its full 1574 kbps (41 ms an
  acknowledgement) and video at 45 to 48 frames a second (about 115
  KB/s), the two trading places when the picture changed a lot (then
  video 200 to 290 KB/s and the sound short).
* Decrypting took **64 to 66 % of the Pico's time** at 310 KB/s (AES-128-
  GCM in mbedTLS on a Cortex-M0+ at 125 MHz: until the 250 MHz run below
  the handshake and stream firmwares did not set the clock, and the
  pico-sdk leaves an RP2040 at 125 MHz; the handshake's 4.0 s and every
  figure before that run are at 125 MHz): about 480 KB/s would take all
  of it.
* Asked for 30 frames a second (value 2), the phone sent 28 to 31 for
  100 s: 15 to 20 KB/s of video on a still map, up to 290 KB/s while
  panning, key frames up to 101 KB.  The sound kept its full rate
  except while panning.
* `SDSTREAMDB.UF2` double-buffers the bulk transfers (two packets a
  transaction).  It first stopped with TinyUSB's "Data Seq Error": after
  a short packet in the first buffer the controller does not use the
  second (no data is lost, the patch counts it and the count stayed 0),
  so the data toggle prepared for the second has to be given back; the
  patch does.  Double-buffered, the USB carried the same **310 to 320
  KB/s** as a packet a transaction.  So the controller is not the limit:
  the firmware reads, then decrypts, then reads again, and with 65 % of
  the time decrypting the link (about 900 KB/s when it runs) stands idle
  the rest.
* So the next read now goes under way as soon as one is in (`aoa.c` has
  two buffers), and is decrypted while the next comes; bulk OUT (the
  acknowledgements, a few dozen bytes) is back on a polled endpoint, so
  that EPX can wait on the phone meanwhile.  With it the USB peaked at
  **404 KB/s** and held about 370 KB/s through long pans: video 28 to
  30 frames a second at 160 to 290 KB/s, and the sound near its full
  192 KB/s, where before the same pans cut it to 30 to 100.  Decrypting
  then took 84 to 91 % of the Pico's time: the decryption (about 440
  KB/s on core 1, which also runs the USB) was the limit.
* **At 250 MHz** (`SYS_KHZ=250000 sdbridge/build.sh`; the firmware now
  sets the clock as `src/main.c` does, 200 MHz by default, at 1.15 V)
  the handshake's slow step took 2.0 s, and the link stopped having a
  ceiling of its own in a minute of Maps: USB up to **660 KB/s**, video
  up to 524 KB/s while panning at 22 to 33 frames a second, and the
  sound short of its rate in 3 seconds out of 80 (25 out of 95 before).
  Decrypting took 34 to 45 % of the time in ordinary use and 79 % at the
  peak: about 830 KB/s would take all of it, near the link's own 900.

Tested on the host (`tests/tls_host.c`): a stand-in phone goes through
the whole session in real frames and encryption: discovery (the
response's channels and video configuration read back), the channels
opened and a fourth refused, driving status, input binding, video
setup, focus and start, a codec configuration (its profile and level
read), a 40 KiB key frame over three frames with one acknowledgement
after the last, an ordinary frame, focus again, a ping and audio focus.

## The phone's video into the slot: `SDLINK.UF2`

The two firmwares in one: the phone's side of `SDSTREAM.UF2` and the
card of `SDBRIDGE.UF2` on the same board, the phone's pictures going
from one to the other.  On the 192 `sdk/examples/aaviewer` reads them,
decodes them (`third_party/h264bsd`) and shows them at half size.
Built with the others when the build is given a certificate
(`AA_CERT`, `AA_KEY`); best at `SYS_KHZ=250000`.

**The format** (`sdk/examples/aaviewer/aalink.h`, kept there because
the SH-4 compiler's container sees only `sdk/`): the card gets a file
`AALINK.BIN` (32 MiB in one run of clusters, after `LINKBENC.BIN`'s
room).  Its first sector says how things stand: where the stream's
blocks begin and end, the last key frame, the codec's configuration
(the H.264 parameter sets).  After eight such sectors comes a window
onto the stream: block `seq` is window sector `seq % 65536`.  A block
is a header (its number, flags, how far the bridge has blocks, a sum)
and 492 bytes of a message, a message (the configuration, or a
picture) starting a block of its own.  A block the bridge does not yet
have reads as "not ready"; one it has reads the same every time until
the 192 starts a read past it, which lets it go.  So a read that went
wrong is made again, and nothing has to be said back to the bridge.

**Pacing.**  A video message is acknowledged to the phone only when its
last block has gone out to the 192 (`aa_hu` with `defer_video_acks`):
with one message ahead of the acknowledgements, the phone sends no
faster than the 192 decodes, and nothing is dropped.  The bridge keeps
128 blocks (63 KB) in RAM; a key frame can be bigger, and goes through
as the 192 reads it: when the ring has no room for another decrypted
frame, the phone's bytes wait in USB (the log's "paused").  Sound is
acknowledged and dropped.  A 192 that starts while the video is under
way, with the last key frame gone, reads the second control sector,
and the bridge then asks the phone for one by taking the video's focus
away and giving it back (a guess at what makes a phone send one: not
yet seen).

**The board's two cores.**  Core 0 does the card's command line and
nothing else, from RAM, with no interrupt enabled: it has 64 of the
host's clocks to answer.  Core 1 runs USB, TLS and the session from
flash, and the card's data side in two interrupts of higher priority:
a new read from core 0 (the inter-core FIFO) and a block gone to the
data state machine (DMA); each makes the next block (`volume_sector`,
`aastream_sector`, the CRC) from RAM.  mbedTLS is too big for the whole
firmware to run from RAM as `SDBRIDGE.UF2` does, so the card's code is
put there by `BRIDGE_RAM` (with no jump tables or library calls made of
its loops); `arm-none-eabi-size` in the build says what is left for
mbedTLS's heap (about 61 KB with 128 blocks).

**Input.**  The 192's knob and keys go to the phone the other way
without a write: each event is a read (CMD17) of one of 4096 sectors
after the window, the sector's number the event (a sequence number, so
that a read made again is not taken twice; a key of the head unit's
and pressed or let go, or the rotary controller's detents).  The bridge
sends it to the phone as Android Auto's input event (aasdk's
InputEventIndication: buttons in field 4, relative input in field 6,
scan code 65536 the rotary controller).  The viewer's mapping: the
select knob turned is the rotary controller and pushed is ENTER, RETURN
pressed briefly BACK, NAV home, MEDIA play/pause, NEXT and PREV the
tracks, the four keys at the screen's corners up, down, right and left;
RETURN held, the display key and CD EJECT stay the unit's.  The steering
wheel (the kit's car driver, `docs/car-bus.md` in rnse-sdk; CAN 0x5C3)
comes in as the same keys: its seek up and down as NEXT and PREV, its
volume keys as the volume knob.  Tested in the emulator
(`viewer_follows_the_steering_wheel`); the wheel's codes themselves were
seen on a unit with the stock software, the viewer with them not yet.
On the 193 too, since kit v0.4.0, whose car driver takes the CAN
controller as the stock software left it (`can_attach`; the kit's
`can_init` left the bench 193's bus silent).

**Sound.**  The phone's sound goes through the same stream: its media
channel (48 kHz stereo), its navigation voice and its own sounds (16 kHz
mono), 16-bit samples as the phone sends them.  The phone interleaves
its channels frame by frame, so the stream does too: every block has
its message's type, a picture's blocks may have sound's between them,
START and END mark each message's own first and last.  The bridge drops
sound a message at a time when the 192 has read nothing for a second or
the ring is nearly full (the pictures are held back for, sound is not).
On the 192 each source has a FIFO; the other card processor than the
bridge's plays a mix of them at 44.1 kHz (`src/pcm.c`'s ring of blocks,
fed by DMA, which plays on while a picture is decoded), each source
resampled to it, its step nudged by how full its FIFO is (some 200 ms
ahead, at most half a per cent of pitch), so that the phone's clock and
the converter's do not drift apart.  The viewer decodes one picture and
then reads again, so that the sound keeps coming; the volume knob sets
the amplifier's volume.  `tests/aaviewer.rs` plays a 1 kHz tone laid in
between the pictures and checks its pitch, its levels and that it has
no break; `tests/disc.rs` hears it through the stock software's path.

**On the 192.**  `AAVIEW.BIN` (`sdk/examples/aaview`, in the SD loader's
menu) loads `AAVIEWER.IMG` (`sdk/out/aaviewer-192.bin`, linked at
0x8c010000 by its file `place`: the decoder makes it too big for the
loader's place, and a `.BIN` of that size would be listed by the loader)
and enters it, and is its way back, as `quakeboot` is for Quake.  The
viewer looks for `AALINK.BIN` in either slot, through the installed
software's card program if it is there and else through its own, then
takes that slot over with `linkbench`'s program (`program.h`): CMD18 in
runs of up to 32 blocks at the card interface's /4 clock, the words
read in bursts, as `linkbench` found clean on a unit.  Each picture is
decoded whole; once one has been read the next read starts at once, a
single block, so that the bridge has its room back while the 192
decodes.  The console has its counts every two seconds (frames a
second, the link's KB/s, milliseconds decoding, showing and reading a
frame, blocks read again).  The display key, CD EJECT and RETURN held
work all the while.

**Tested here** (2026-11-18): `sh sdbridge/tests/run.sh` (the ring:
blocks, "not ready", the room, letting go, the window's numbers, the
file in the volume; the session's held-back acknowledgements);
`tests/aaviewer.rs` (no firmware: the bridge's card as its own code
makes it, with `tests/data/margin.264` laid in as a phone's messages,
read and decoded by the viewer on the emulated 192, every frame's
CRC-32 as ffmpeg's); `tests/disc.rs` `android_auto_viewer_from_the_sd_loader`
(the same started by the stock software and the SD loader, through
`AAVIEW.BIN`, and back with RETURN held).  The emulated figures: 63 ms
decoding and 39 ms showing a frame of that stream, 2 ms reading it.
**On the unit** (2026-11-18, evening): the board with `SDLINK.UF2` in
the second slot, the phone on its USB port, the viewer started from the
loader's menu: **Google Maps from the phone on the 192's screen**
(photo), the whole path working the first time.  Its colours were off:
washed out, the map's light greys pale lilac with speckles of pink
(the 3-3-2 palette and the colour conversion).  The same evening:

* 10 to 13 pictures a second from the phone through the slot (the
  board's log: 120-150 KB/s out, decrypting about 30 % of the time, its
  slowest answer 56 of the host's clocks, no bad CRC).
* The colour: the viewer's table now has its chroma steps centred on 128
  (h264bench's took the middle of steps from 0, so that every grey came
  out with red and blue up and green down), the stream's own range and
  matrix, and a palette of a 6x6x6 cube and 40 greys, each entry the
  nearest.  Maps is mostly greys now, as it should be.
* A crash: the line saying the stream's colours overran a buffer of 48
  bytes on the stack, and the unit reset to its own software at the
  first pictures (the test stream says nothing of its colours, so the
  emulator never printed it).  The line is said once in every stream
  now, so the tests print it too.
* The keys: the viewer read the faceplate's line only between pictures;
  it is now served every few milliseconds, from a hook in the decoder
  (`h264bsdIdle`) and from the drawing loop.  The display key, CD EJECT
  and RETURN held worked throughout.
* The knob and keys for the phone work.  The first try let go of
  pictures at every turn of the knob (the input sectors were taken as
  the window's; `aastream_in_window` now, with a test), the 192 then
  asked for a key frame and the phone's video began anew: a flash.  The
  knob felt slow: its events now go during decoding and drawing, and the
  debug console's lines (9600 bit/s, waited for: some 130 ms for a
  report) are for the test build only.

**Running it.**  As for `SDSTREAM.UF2` (5 V into pin 40, the phone
through an OTG adapter, the log on GP0 at 115200 baud: a USB-serial
adapter or the Saleae), with the board's card end in the 192's second
slot as for `SDBRIDGE.UF2` (only the ground joined), `AAVIEW.BIN` and
`AAVIEWER.IMG` on a card in the first.  Start the viewer from the
loader's menu, then plug the phone in.  The board's log has a line a
second: the card's reads and KB/s, the phone's pictures a second, the
ring, acknowledgements, pauses.

## Next

Find out what the unit makes of it: the frames a second with a real
phone's stream, whether the board answers in time with TLS running,
and whether asking for a key frame works.  Then faster showing on the
192 (a third of a frame's time), the touch screen (the knob as a
pointer) and the way back to the phone (writes, CMD24/25), and sound
through a card processor.
