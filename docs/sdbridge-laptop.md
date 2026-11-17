# Carrying on with the Android Auto bridge on your own computer

Everything for the bridge is on the branch `ccr-249b75e2-puy2ij`
(pull request #27).  This page is how to get it onto a laptop, build
every part, run the tests, flash the boards, and where the work stands.
The details of each part are in [sdbridge.md](sdbridge.md) (the board)
and [android-auto.md](android-auto.md) (the 192's side and the
measurements).

## Getting the code

```sh
git clone https://github.com/jroark/rnse.git
cd rnse
git checkout ccr-249b75e2-puy2ij
```

Once pull request #27 is merged, `main` has all of it instead.

## Tools

| For | macOS (Homebrew) | Debian / Ubuntu |
|---|---|---|
| the emulator and its tests | `curl https://sh.rustup.rs -sSf \| sh` (Rust) | the same |
| the RP2040 firmware | `brew install cmake` and `brew install --cask gcc-arm-embedded` (the Arm toolchain with newlib) | `sudo apt install cmake gcc-arm-none-eabi libnewlib-arm-none-eabi build-essential` |
| the 192's programs (SH-4) | Docker Desktop, then `PATH=$PWD/tools/sh4-docker:$PATH` | `sudo apt install gcc-sh4-linux-gnu binutils-sh4-linux-gnu` |
| test certificates, host tests | `openssl` and `python3` (both already there) | `sudo apt install openssl python3` |
| the serial log | a USB serial adapter (or a second Pico with debugprobe) and `screen` or `minicom` | the same |

`sdbridge/build.sh` clones the pico-sdk 2.1.1 with TinyUSB and mbedTLS
into `sdbridge/work/` the first time (git, a few hundred MB), and checks
their commits.  Nothing it downloads is committed.

## Building

```sh
cargo build --release                       # the emulator
sdk/build.sh 192 linkbench                  # the slot benchmark for the 192: sdk/out/linkbench-192.bin
sdbridge/build.sh                           # the board's firmware, see below
```

`sdbridge/build.sh` makes, for the RP2040-Zero in `sdbridge/out/` and
for the Raspberry Pi Pico in `sdbridge/out/pico/`:

| File | What it is |
|---|---|
| `SDBRIDGE.UF2` | the board as a read-only SD card for the 192 (`LINKTEST.BIN` on it), for `linkbench` |
| `SDPHONE.UF2` | the phone on the desk: accessory mode and Android Auto's version exchange (works: seen 2026-11-17) |
| `SDHANDSHAKE.UF2` | the TLS handshake with the certificate it is given; only built when given one (below) |
| `SDSTREAM.UF2` | the same, then a head unit: the phone's video counted and acknowledged, its rate logged each second; built with `SDHANDSHAKE.UF2` |

### The test certificate for `SDHANDSHAKE.UF2`

Make it outside the repository, then pass both files to the build:

```sh
mkdir -p ~/rnse-test-cert && cd ~/rnse-test-cert
openssl req -x509 -newkey rsa:2048 -nodes -days 365 \
    -subj "/CN=rnse test head unit" -keyout test-hu.key -out test-hu.crt
cd -
AA_CERT=~/rnse-test-cert/test-hu.crt AA_KEY=~/rnse-test-cert/test-hu.key sdbridge/build.sh
```

The phone refuses a self-signed certificate; the test shows how far the
handshake gets and how long it takes on the board.  The build writes the
key into `sdbridge/work/hu_cert.h` and the UF2: neither may be
committed, and `.gitignore` keeps `sdbridge/work/`, `*.UF2`, `*.key`,
`*.crt`, `*.pem` and `*.der` out.

## Testing without hardware

```sh
sh sdbridge/tests/run.sh                    # the board's code on the host
cargo test --release --test linkbench       # linkbench on the emulated 192, and the board's volume through it
cargo test --release                        # the emulator's unit tests
```

`sdbridge/tests/run.sh` runs three things: the card's commands, the
made-up volume, the checksums and Android Auto's framing (`host.c`); the
TLS code against a stand-in phone (`tls_host.c`, needs the mbedTLS
`sdbridge/build.sh` fetched, and openssl); and the PIO programs in a
simulator against an SD host's waveforms (`piosim.py`, needs
`sdbridge/out/sd.pio.h` from a build).  The `linkbench` tests need an
SH-4 GCC on the PATH; without one they pass without running.

## Flashing a board

Hold the board's BOOT/BOOTSEL button while plugging it into the laptop,
and copy the UF2 onto the `RPI-RP2` drive that appears.  For a Pico, use
the files in `sdbridge/out/pico/`.

**Then unplug it from the laptop before powering it any other way.**
For the phone tests the board is powered with 5 V on VBUS (a Pico's pin
40, ground on pin 38) and the phone hangs off the board's USB socket
through an OTG adapter: two supplies on one wire would fight.

The serial log of `SDPHONE.UF2` and `SDHANDSHAKE.UF2`: the adapter's RX
on the board's GP0 (a Pico's pin 1), ground on pin 3, 115200 baud:

```sh
screen /dev/tty.usbserial-* 115200          # macOS (the name varies)
screen /dev/ttyUSB0 115200                  # Linux
```

## Where it stands

* `linkbench` (the 192): written and run in the emulator, where the
  card interface's modelled rate is the limit (2.1 MB/s at the stock
  clock).  **Not yet run on the unit.**  That run gives the real rate
  of a slot.
* The board as a card (`SDBRIDGE.UF2`): written; its card logic, volume
  and PIO timing tested on the host and in the simulator.  **Not yet run
  in the unit's slot.**
* The phone on the desk (`SDPHONE.UF2`): **works** on a Pico with a
  real phone: accessory mode, Android Auto's version exchange.
* The TLS handshake (`SDHANDSHAKE.UF2`): written and tested on the host
  against a stand-in phone.  **Not yet run on the board.**  With a test
  certificate the real phone ends it with an alert by design.
* The phone's video (`SDSTREAM.UF2`): **works** on a Pico with a real
  phone (2026-11-16, with a certificate the phone accepts): Android
  Auto projects at 800x480, H.264 Baseline 3.1, sound at its full rate
  and video at up to 48 frames a second, through about 310 KB/s of USB
  once TinyUSB's host runs bulk on EPX (`patches/`).  Numbers in
  [sdbridge.md](sdbridge.md).

## What to do next

1. Ask the phone for 30 frames a second (value 2 in the video
   configuration), and see how far USB goes with EPX double-buffered.
2. Run `linkbench` on the unit with an ordinary card and `LINKTEST.BIN`
   (`python3 tools/linktest.py > LINKTEST.BIN`): the slot's real rates.
3. Run `SDBRIDGE.UF2` in the slot on the ribbon (wiring in
   [sdbridge.md](sdbridge.md)) and `linkbench` against it: the bridge
   as a card, and its "slowest answer" in the log.
4. Then the writes into the card (CMD24/25), the two directions through
   the slot, and the two firmwares in one.

Before running anything on the unit: keep the display open while the
ribbon is in, lay the cable clear of the display's motor and the drive,
and keep CD eject and the display key working, as `CLAUDE.md` requires
of every program for the unit.

## With Claude Code on the laptop

Run `claude` in the repository: `CLAUDE.md` is read at the start and
points to these documents.  Telling it which step above you are on, and
pasting the board's log, is enough to carry on.
