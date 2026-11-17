#!/bin/sh
# The bridge's tests that need no board:   sh sdbridge/tests/run.sh
#
# host.c: the card's commands and responses, the made-up volume, the
# checksums, on the host's C compiler.  piosim.py: the PIO programs as
# pioasm made them (sdbridge/out/sd.pio.h, from sdbridge/build.sh)
# against an SD host's waveforms.  tls_host.c: TLS in Android Auto's
# frames against a stand-in phone.  tests/linkbench.rs has another, with
# the volume on the emulated 192.
set -e
here=$(cd "$(dirname "$0")" && pwd)
src=$here/../src
mkdir -p "$here/../work"
${CC:-cc} -O2 -Wall -Wextra -o "$here/../work/host" "$here/host.c" "$src/card.c" "$src/volume.c" "$src/crc.c" "$src/aap.c"
"$here/../work/host"
# TLS inside Android Auto's frames (tests/tls_host.c), on the pico-sdk's
# mbedTLS (sdbridge/build.sh fetches it) and certificates made here, for
# the test only, by OpenSSL.
mbedtls=${PICO_SDK_PATH:-$here/../work/pico-sdk}/lib/mbedtls
if [ -f "$mbedtls/library/aes.c" ] && command -v openssl >/dev/null 2>&1; then
    certs=$here/../work/tls-test
    mkdir -p "$certs"
    for who in hu phone other; do
        [ -f "$certs/$who.crt" ] || openssl req -x509 -newkey rsa:2048 -nodes -days 365 -subj "/CN=rnse test $who" \
            -keyout "$certs/$who.key" -out "$certs/$who.crt" 2>/dev/null
    done
    ${CC:-cc} -O2 -DAA_TLS_HOST_TEST '-DMBEDTLS_CONFIG_FILE="mbedtls_config.h"' -I"$src" -I"$mbedtls/include" \
        -o "$here/../work/tls_host" "$here/tls_host.c" "$src/aa_tls.c" "$src/aap.c" "$mbedtls"/library/*.c
    "$here/../work/tls_host" "$certs"
else
    echo "tls: no mbedTLS in the pico-sdk (run sdbridge/build.sh) or no openssl: not run"
fi
if [ -f "$here/../out/sd.pio.h" ]; then
    python3 -I "$here/piosim.py" "$here/../out/sd.pio.h"
else
    echo "piosim: no sdbridge/out/sd.pio.h (run sdbridge/build.sh): not run"
fi
