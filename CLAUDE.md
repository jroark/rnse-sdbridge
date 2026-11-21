# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

The SD bridge: firmware for an RP2040-Zero (pico-sdk, cloned pinned by `build.sh`, never committed) that answers the Audi RNS-E 192 (SH7750R, variants `V220`/`V222`) as a read-only SD card made up as it is read, and the 192's programs at the other end of that link: `linkbench` (times the link, its words checked against `LINKTEST.BIN`), `h264bench`, and the Android Auto viewer (`aaview` loads `aaviewer`, which decodes the phone's video from `AALINK.BIN`). The 192's programs are built on the kit, rnse-sdk (https://github.com/jroark/rnse-sdk; `RNSE_SDK`, default `../rnse-sdk`), and tested on the emulator, rnse (https://github.com/jroark/rnse). It was split out of rnse with its history. `docs/sdbridge.md` (wiring, what is tested), `docs/sdbridge-laptop.md` (tools, builds, flashing, where the work stands), `docs/android-auto.md`.

Firmware is copyrighted and must never be committed. `.gitignore` blocks `*.iso`, `*.bin`, `*.KWI`, `*.MOT`, `/firmware/`, `/out/`; do not add firmware-derived files (flash dumps, extracted modules, SD card images) to the repository under other names either.
Android Auto's head unit certificate and key, and any other key or certificate, stay outside every repository (`SDHANDSHAKE.UF2` is built only with `AA_CERT`/`AA_KEY` pointing outside it); so do the bridge's logs (`work/`) and builds (`out/`, `*.UF2`).

## Mandatory hardware behavior

- **Always preserve physical ejection.** Every custom firmware, loader,
  diagnostic and test payload must support opening/folding down the display
  and ejecting the disc. Keep faceplate polling and eject handling responsive
  on normal screens, error screens and during long-running operations. Do not
  repurpose the display/open key without another accessible physical way to
  open the display and reach the disc-eject control. SDK `KEY_DISPLAY` is
  emulator `EJECT`; `KEY_CD_EJECT` is a separate key. Verify the actual eject
  command path before providing hardware test builds, including across handoffs.
- **Hold RETURN for two seconds to return to the loader.** Every payload
  launched by the SD loader must implement this shortcut from every screen,
  close active apps and stop playback before calling `loader_return(status)`.
  A short RETURN retains its normal back/cancel behavior; a long hold must not
  also trigger that short action or immediately relaunch the payload after
  returning. Return directly to the resident loader without resetting or
  invoking an updater. Outside a loader session, report that return is
  unavailable and remain operable, with eject still working.
- **No firmware-flash option unless the user explicitly asks for it.** Default
  custom loaders, test payloads and UIs must neither flash firmware nor expose
  any updater/install/restore/erase option or hidden button sequence that can
  do so. This includes double-SETUP, `stock_update()`, firmware removal and
  automatic updater handoffs. Permission to build, test, recover, return to
  the loader or eject a disc is not permission to flash. Existing updater
  mechanisms described elsewhere in this file are technical background, not
  authorization to expose or invoke them. Include a flash option only when
  explicitly requested by the user.

Hardware incident reported 2026-11-15: double-SETUP on the original loader
invoked an updater and flashed firmware whose eject button did not work. The
disc was trapped and the user had to disassemble the RNS-E. These requirements
apply to all future hardware builds.
## Commands

```sh
./build.sh                                  # out/SDBRIDGE.UF2, SDPHONE.UF2, SDLINK.UF2 (pico-sdk cloned, pinned)
sh tests/run.sh                             # card logic on the host, PIO programs in a simulator (tests/piosim.py)
sh ../rnse-sdk/build.sh 192 ./linkbench     # the 192's programs: linkbench, h264bench, aaview, aaviewer
cargo test --release                        # tests/linkbench.rs, tests/aaviewer.rs (no firmware); tests/disc.rs skips without RNSE_DISC
RNSE_DISC=$PWD/disc.iso cargo test --release --test disc -- --nocapture
```

`tests/disc.rs` needs `RNSE_DISC` (an update disc image, firmware: never committed) and an SH-4 GCC on the PATH (or the kit's `tools/sh4-docker`); without them it returns early and reports success, which proves nothing. A crate's tests run in its own directory, so give these variables absolute paths.

## Layout

- `src/`: the RP2040 firmwares (`main.c` the card, `card.c`, `volume.c`; `sdphone.c`, `aap.c`; `sdhandshake.c`, `aa_tls.c`; `sdlink.c`, `aastream.c`), `CMakeLists.txt`, `build.sh`, `patches/` (TinyUSB), `link.py`, `uf2.py`, `pem2c.py`.
- `linkbench/`, `h264bench/`, `aaview/`, `aaviewer/` (`aalink.h` is the format of `AALINK.BIN`; its file `place` links it outside the 192's loader place): the 192's programs; `aaviewer/` runs on the 193 too (`docs/android-auto.md`, "The 193"); `third_party/h264bsd` (a `libs` entry of h264bench and aaviewer).
- `tests/`: `run.sh`, `host.c`, `tls_host.c`, `piosim.py`; the emulator tests (crate `rnse-sdbridge`; `rnse` pinned in `Cargo.toml` and `Cargo.lock`); `data/margin.*` (synthetic H.264 frames made by `tools/aa-streams.sh`, and ffmpeg's CRCs of them). `tools/linktest.py` checks the link test's words.

A `docs/....md` named in a comment that is not in this repository is in rnse (`docs/hardware.md`, "SD card processors", is the one that matters here) or, for the kit's own (`docs/custom-firmware.md`, `docs/sd-loader.md`), in rnse-sdk; `sdk/src/...` is the kit's `src/...`.

## Things that are easy to miss

- **The card is read by the 192's card processors**, running the installed software's program for them, not by the SH-4: what the bridge must answer, and how fast, is set by that program (`docs/hardware.md` in rnse, "SD card processors"). Timings measured on a unit are in `docs/sdbridge.md`; keep what was measured apart from what is assumed.
- Programs on the 192 must keep EJECT and the display working while they wait on the link: a stalled bridge must not trap the disc.
