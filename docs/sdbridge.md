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

## Next

Writes (CMD24 and CMD25) for the way back to the phone, and a mailbox
of our own on top of reads and writes (sector addresses as channels);
then the two firmwares in one, the phone's bytes carried through the
slot, and the protocol itself (TLS and the channels) on the 192.  The RP2040's USB is full speed only
(12 Mbit/s): whether a phone's Android Auto is content with that is the
question of that step.
