#!/bin/sh
# The bridge's tests that need no board:   sh sdbridge/tests/run.sh
#
# host.c: the card's commands and responses, the made-up volume, the
# checksums, on the host's C compiler.  piosim.py: the PIO programs as
# pioasm made them (sdbridge/out/sd.pio.h, from sdbridge/build.sh)
# against an SD host's waveforms.  tests/linkbench.rs has a third, with
# the volume on the emulated 192.
set -e
here=$(cd "$(dirname "$0")" && pwd)
src=$here/../src
mkdir -p "$here/../work"
${CC:-cc} -O2 -Wall -Wextra -o "$here/../work/host" "$here/host.c" "$src/card.c" "$src/volume.c" "$src/crc.c"
"$here/../work/host"
if [ -f "$here/../out/sd.pio.h" ]; then
    python3 -I "$here/piosim.py" "$here/../out/sd.pio.h"
else
    echo "piosim: no sdbridge/out/sd.pio.h (run sdbridge/build.sh): not run"
fi
