#!/bin/sh
# Build the SD bridge for a Waveshare RP2040-Zero and a Raspberry Pi Pico:
#   sdbridge/build.sh
#
# The result is sdbridge/out/: SDBRIDGE.UF2, the card, and SDPHONE.UF2,
# the phone on the desk, for the board's USB drive (hold BOOT while
# plugging it in, copy the file onto RPI-RP2), and their .elf files for
# a debugger; sdbridge/out/pico/ the same for a Raspberry Pi Pico.  docs/sdbridge.md says how to wire it.
#
# The pico-sdk (BSD-3) and the TinyUSB it uses (MIT) are not kept in
# the repository: they are cloned, pinned and checked against their
# commits, into sdbridge/work/, and TinyUSB gets sdbridge/patches/.  PICO_SDK_PATH=DIR takes an SDK from
# elsewhere (not checked).  Needs git, cmake, a host C compiler (for the
# SDK's pioasm) and arm-none-eabi-gcc with newlib.
#
# SYS_KHZ=250000 for a faster processor clock (200 MHz by default).
# AA_CERT=CERT.pem AA_KEY=KEY.pem (both outside the repository) builds
# the handshake desk test too, SDHANDSHAKE.UF2, and the video stream's,
# SDSTREAM.UF2, and the bridge whole, SDLINK.UF2 (docs/sdbridge.md);
# AA_MAX_UNACKED=N for the frames the phone may send ahead of the
# acknowledgements (1).
set -e
here=$(cd "$(dirname "$0")" && pwd)
work=$here/work
out=$here/out

sdk_tag=2.1.1
sdk_commit=bddd20f928ce76142793bef434d4f75f4af6e433
tusb_commit=86ad6e56c1700e85f1c5678607a762cfe3aa2f47
mbedtls_commit=5a764e5555c64337ed17444410269ff21cb617b1

mkdir -p "$work" "$out"
if [ -z "$PICO_SDK_PATH" ]; then
    PICO_SDK_PATH=$work/pico-sdk
    if [ ! -d "$PICO_SDK_PATH/.git" ]; then
        git -c advice.detachedHead=false clone -q --depth 1 --branch "$sdk_tag" https://github.com/raspberrypi/pico-sdk.git "$PICO_SDK_PATH"
    fi
    [ "$(git -C "$PICO_SDK_PATH" rev-parse HEAD)" = "$sdk_commit" ] || { echo "pico-sdk: not commit $sdk_commit" >&2; exit 1; }
    if [ ! -f "$PICO_SDK_PATH/lib/mbedtls/library/aes.c" ]; then
        git -C "$PICO_SDK_PATH" submodule update -q --init --depth 1 lib/mbedtls
    fi
    [ "$(git -C "$PICO_SDK_PATH/lib/mbedtls" rev-parse HEAD)" = "$mbedtls_commit" ] || { echo "mbedtls: not commit $mbedtls_commit" >&2; exit 1; }
    if [ ! -f "$PICO_SDK_PATH/lib/tinyusb/src/tusb.h" ]; then
        git -C "$PICO_SDK_PATH" submodule update -q --init --depth 1 lib/tinyusb
    fi
    [ "$(git -C "$PICO_SDK_PATH/lib/tinyusb" rev-parse HEAD)" = "$tusb_commit" ] || { echo "tinyusb: not commit $tusb_commit" >&2; exit 1; }
    # TinyUSB's RP2040 host runs bulk endpoints on the controller's polled
    # "interrupt" endpoints, a 64-byte packet a millisecond (64 KB/s, seen
    # with a phone): patches/ runs them on EPX instead (docs/sdbridge.md).
    for p in "$here"/patches/tinyusb-*.patch; do
        git -C "$PICO_SDK_PATH/lib/tinyusb" apply --reverse --check "$p" 2>/dev/null ||
            git -C "$PICO_SDK_PATH/lib/tinyusb" apply "$p" || { echo "tinyusb: $p does not apply" >&2; exit 1; }
    done
fi

# The handshake desk test only with a certificate and key from outside
# the repository (docs/sdbridge.md: a self-signed test one will do, and
# the phone will refuse it).  The header made of them, and the UF2s, hold
# the key.
cert_cmake=""
if [ -n "$AA_CERT" ] || [ -n "$AA_KEY" ]; then
    [ -n "$AA_CERT" ] && [ -n "$AA_KEY" ] || { echo "AA_CERT and AA_KEY go together" >&2; exit 1; }
    python3 -I "$here/pem2c.py" "$AA_CERT" "$AA_KEY" "$work/hu_cert.h"
    cert_cmake=-DAA_CERT_HEADER=$work/hu_cert.h
else
    rm -f "$work/hu_cert.h"
fi

# Twice: for the RP2040-Zero (sdbridge/out/) and for a Raspberry Pi Pico
# (sdbridge/out/pico/), whose LED is a plain one on GP25.
for board in waveshare_rp2040_zero pico; do
    case $board in
        pico) build=$work/build-pico dest=$out/pico ;;
        *) build=$work/build dest=$out ;;
    esac
    mkdir -p "$dest"
    cmake -S "$here" -B "$build" -DPICO_SDK_PATH="$PICO_SDK_PATH" -DPICO_BOARD=$board \
        -DCMAKE_BUILD_TYPE=Release -DSYS_KHZ=${SYS_KHZ:-200000} -UAA_CERT_HEADER $cert_cmake \
        -DAA_MAX_UNACKED=${AA_MAX_UNACKED:-1} >/dev/null
    cmake --build "$build" -j4
    python3 -I "$here/uf2.py" "$build/sdbridge.bin" "$dest/SDBRIDGE.UF2"
    python3 -I "$here/uf2.py" "$build/sdphone.bin" "$dest/SDPHONE.UF2"
    if [ -n "$cert_cmake" ]; then
        python3 -I "$here/uf2.py" "$build/sdhandshake.bin" "$dest/SDHANDSHAKE.UF2"
        python3 -I "$here/uf2.py" "$build/sdstream.bin" "$dest/SDSTREAM.UF2"
        python3 -I "$here/uf2.py" "$build/sdstreamdb.bin" "$dest/SDSTREAMDB.UF2"
        python3 -I "$here/uf2.py" "$build/sdlink.bin" "$dest/SDLINK.UF2"
        arm-none-eabi-size "$build/sdstream.elf" "$build/sdhandshake.elf" "$build/sdlink.elf" | sed "s|$build/||"
    else
        rm -f "$dest/SDHANDSHAKE.UF2" "$dest/SDSTREAM.UF2" "$dest/SDSTREAMDB.UF2" "$dest/SDLINK.UF2"
    fi
    cp "$build/sdbridge.elf" "$dest/sdbridge.elf"
    cp "$build/sdphone.elf" "$dest/sdphone.elf"
done
cp "$work/build/sd.pio.h" "$out/sd.pio.h"
ls -l "$out"/*.UF2 "$out"/pico/*.UF2
