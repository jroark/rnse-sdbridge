#!/bin/sh
# The probe (esp32/probe): esp32/probe.sh [idf.py arguments], as
# esp32/build.sh but with no certificate; its build is in work/esp32.
set -e
here=$(cd "$(dirname "$0")" && pwd)
work=$here/../work/esp32
: "${IDF_PATH:=$HOME/esp/esp-idf}"
export IDF_PATH
[ -f "$IDF_PATH/export.sh" ] || { echo "no ESP-IDF at $IDF_PATH" >&2; exit 1; }
mkdir -p "$work"
. "$IDF_PATH/export.sh" >/dev/null
[ $# -gt 0 ] || set -- build
exec idf.py -C "$here/probe" -B "$work/probe-build" -DSDKCONFIG="$work/probe-sdkconfig" "$@"
