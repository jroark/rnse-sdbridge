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

## Next

Writes (CMD24 and CMD25) for the way back to the phone, and a mailbox
of our own on top of reads and writes (sector addresses as channels);
then the USB host for the phone (Android Open Accessory) on the board's
connector, which the log uses now.  The RP2040's USB is full speed only
(12 Mbit/s): whether a phone's Android Auto is content with that is the
question of that step.
