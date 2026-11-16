#!/bin/sh
# Build the SD bridge for a Waveshare RP2040-Zero:   sdbridge/build.sh
#
# The result is sdbridge/out/: SDBRIDGE.UF2 for the board's USB drive
# (hold BOOT while plugging it in, copy the file onto RPI-RP2) and
# sdbridge.elf for a debugger.  docs/sdbridge.md says how to wire it.
#
# The pico-sdk (BSD-3) and the TinyUSB it uses (MIT) are not kept in
# the repository: they are cloned, pinned and checked against their
# commits, into sdbridge/work/.  PICO_SDK_PATH=DIR takes an SDK from
# elsewhere (not checked).  Needs git, cmake, a host C compiler (for the
# SDK's pioasm) and arm-none-eabi-gcc with newlib.
#
# SYS_KHZ=250000 for a faster processor clock (200 MHz by default).
set -e
here=$(cd "$(dirname "$0")" && pwd)
work=$here/work
out=$here/out

sdk_tag=2.1.1
sdk_commit=bddd20f928ce76142793bef434d4f75f4af6e433
tusb_commit=86ad6e56c1700e85f1c5678607a762cfe3aa2f47

mkdir -p "$work" "$out"
if [ -z "$PICO_SDK_PATH" ]; then
    PICO_SDK_PATH=$work/pico-sdk
    if [ ! -d "$PICO_SDK_PATH/.git" ]; then
        git -c advice.detachedHead=false clone -q --depth 1 --branch "$sdk_tag" https://github.com/raspberrypi/pico-sdk.git "$PICO_SDK_PATH"
    fi
    [ "$(git -C "$PICO_SDK_PATH" rev-parse HEAD)" = "$sdk_commit" ] || { echo "pico-sdk: not commit $sdk_commit" >&2; exit 1; }
    if [ ! -f "$PICO_SDK_PATH/lib/tinyusb/src/tusb.h" ]; then
        git -C "$PICO_SDK_PATH" submodule update -q --init --depth 1 lib/tinyusb
    fi
    [ "$(git -C "$PICO_SDK_PATH/lib/tinyusb" rev-parse HEAD)" = "$tusb_commit" ] || { echo "tinyusb: not commit $tusb_commit" >&2; exit 1; }
fi

cmake -S "$here" -B "$work/build" -DPICO_SDK_PATH="$PICO_SDK_PATH" -DPICO_BOARD=waveshare_rp2040_zero \
    -DCMAKE_BUILD_TYPE=Release ${SYS_KHZ:+-DCMAKE_C_FLAGS=-DSYS_KHZ=$SYS_KHZ} >/dev/null
cmake --build "$work/build" -j4
python3 -I "$here/uf2.py" "$work/build/sdbridge.bin" "$out/SDBRIDGE.UF2"
cp "$work/build/sdbridge.elf" "$out/sdbridge.elf"
cp "$work/build/sd.pio.h" "$out/sd.pio.h"
ls -l "$out/SDBRIDGE.UF2"
