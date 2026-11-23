#!/bin/sh
# Builds tools/aa_tcp.c (Android Auto over TCP from this machine) into
# work/aa_tcp, on the pico-sdk's mbedTLS (build.sh fetches it;
# PICO_SDK_PATH for one elsewhere) and the head unit certificate and key
# given, which stay outside the repository:
#
#   AA_CERT=~/rnse-hu-cert/headunit.crt AA_KEY=~/rnse-hu-cert/headunit.key sh tools/aa_tcp.sh
#   work/aa_tcp connect 192.168.1.23        # the phone's head unit server
set -e
here=$(cd "$(dirname "$0")" && pwd)
src=$here/../src
work=$here/../work
[ -n "$AA_CERT" ] && [ -n "$AA_KEY" ] || { echo "AA_CERT and AA_KEY: the head unit's certificate and key (PEM)" >&2; exit 1; }
mbedtls=${PICO_SDK_PATH:-$work/pico-sdk}/lib/mbedtls
[ -f "$mbedtls/library/aes.c" ] || { echo "no mbedTLS at $mbedtls: run build.sh, or set PICO_SDK_PATH" >&2; exit 1; }
mkdir -p "$work/aa_tcp.d"
python3 -I "$here/../pem2c.py" "$AA_CERT" "$AA_KEY" "$work/aa_tcp.d/hu_cert.h" >/dev/null
${CC:-cc} -O2 -Wall -Wextra -Wno-unused-parameter '-DMBEDTLS_CONFIG_FILE="mbedtls_config.h"' \
    -I"$src" -I"$mbedtls/include" -I"$work/aa_tcp.d" \
    -o "$work/aa_tcp" "$here/aa_tcp.c" "$src/aa_session.c" "$src/aa_tls.c" "$src/aa_hu.c" "$src/aap.c" "$mbedtls"/library/*.c
echo "$work/aa_tcp"
