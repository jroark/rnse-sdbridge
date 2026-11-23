#!/bin/sh
# The card test (esp32/card): esp32/probe.sh [idf.py arguments], as
# esp32/build.sh but with no certificate; its build is in work/esp32.
set -e
here=$(cd "$(dirname "$0")" && pwd)
work=$here/../work/esp32
: "${IDF_PATH:=$HOME/esp/esp-idf}"
export IDF_PATH
[ -f "$IDF_PATH/export.sh" ] || { echo "no ESP-IDF at $IDF_PATH" >&2; exit 1; }
mkdir -p "$work"
. "$IDF_PATH/export.sh" >/dev/null
# CARD_PROGRAM: a program of the 192's for the card's LINKBENC.BIN (the
# SD loader runs it), or none.
if [ -n "$CARD_PROGRAM" ]; then cp "$CARD_PROGRAM" "$work/card_program.bin"; else printf 'none' > "$work/card_program.bin"; fi
[ $# -gt 0 ] || set -- build
exec idf.py -C "$here/card" -B "$work/card-build" -DSDKCONFIG="$work/card-sdkconfig" -DCARD_PROGRAM_DIR="$work" "$@"
