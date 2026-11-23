#!/bin/sh
# The ESP32-S3's firmware:
#
#   AA_CERT=~/rnse-hu-cert/headunit.crt AA_KEY=~/rnse-hu-cert/headunit.key esp32/build.sh [idf.py arguments]
#   ... esp32/build.sh -p /dev/cu.usbmodemXXXX flash monitor
#
# on ESP-IDF v5.5 (IDF_PATH, default ~/esp/esp-idf, installed with its
# install.sh esp32s3).  Everything it makes is in work/esp32 (the build,
# sdkconfig with the Wi-Fi password if one is set by `menuconfig`, and
# hu_cert.h, which holds the key): none of it in the repository.
set -e
here=$(cd "$(dirname "$0")" && pwd)
work=$here/../work/esp32
[ -n "$AA_CERT" ] && [ -n "$AA_KEY" ] || { echo "AA_CERT and AA_KEY: the head unit's certificate and key (PEM)" >&2; exit 1; }
: "${IDF_PATH:=$HOME/esp/esp-idf}"
export IDF_PATH
[ -f "$IDF_PATH/export.sh" ] || { echo "no ESP-IDF at $IDF_PATH" >&2; exit 1; }
mkdir -p "$work/cert"
python3 -I "$here/../pem2c.py" "$AA_CERT" "$AA_KEY" "$work/cert/hu_cert.h" >/dev/null
. "$IDF_PATH/export.sh" >/dev/null
[ $# -gt 0 ] || set -- build
exec idf.py -C "$here" -B "$work/build" -DSDKCONFIG="$work/sdkconfig" -DAA_CERT_DIR="$work/cert" "$@"
