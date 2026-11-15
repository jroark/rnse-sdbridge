#!/bin/sh
# Test streams for sdk/examples/h264bench: H.264 as an Android Auto
# phone sends it to a head unit that asks for 800x480 (constrained
# baseline, CAVLC, one reference frame, no B frames, 30 frames a
# second), made of synthetic pictures, so nothing here is anyone's
# recording.  Needs ffmpeg with libx264.
#
#     tools/aa-streams.sh DIR [SECONDS [PROGRAM]]
#
# writes into DIR:
#   MAP.264     a detailed picture panned slowly, as a moving map is
#   LIST.264    a still screen with a highlight bar moving down a list
#   FULL.264    motion over the whole picture (ffmpeg's testsrc2): the worst case
#   MARGIN.264  MAP drawn into the top left 400x240 only, the rest black:
#               what the phone sends when the head unit gives margins
#               that leave a 400x240 picture
#   *.CRC       for each, after every decoded frame (ffmpeg's decoder)
#               the CRC-32 of the frames up to it, one after the other,
#               8 hex digits a line, as h264bench prints them; the last
#               line is that of the whole stream
# and a card image, CARD.IMG, with the four streams (rnse card) and,
# if given, PROGRAM as RNSE.BIN for examples/sdboot.
set -e
dir=${1:?usage: aa-streams.sh DIR [SECONDS]}
secs=${2:-5}
mkdir -p "$dir"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

enc="-c:v libx264 -profile:v baseline -level 3.1 -pix_fmt yuv420p -r 30 -bf 0 -refs 1
     -x264-params keyint=300:min-keyint=300:scenecut=0:repeat-headers=1 -an -f h264"

# A large still with flat areas and fine detail, to pan over.
ffmpeg -loglevel error -y -f lavfi -i "mandelbrot=s=1800x1100:end_pts=1:start_scale=1.5:end_scale=1.5" \
    -frames:v 1 "$tmp/still.png"

ffmpeg -loglevel error -y -loop 1 -framerate 30 -i "$tmp/still.png" -t "$secs" \
    -vf "crop=800:480:x='100+t*90':y='80+t*40'" $enc -b:v 2M "$dir/MAP.264"

ffmpeg -loglevel error -y -f lavfi -i "color=c=0x1c2430:s=800x480:r=30:d=$secs" \
    -vf "drawgrid=w=800:h=60:t=2:c=0x46505a,drawbox=x=0:y=0:w=800:h=56:c=0x303c48:t=fill,drawbox=x=0:y='60+60*mod(floor(t*2)\,7)':w=800:h=60:c=0x2060a0@0.8:t=fill,drawbox=x=40:y=12:w=200:h=32:c=0xd0d0d0:t=fill" \
    $enc -b:v 1M "$dir/LIST.264"

ffmpeg -loglevel error -y -f lavfi -i "testsrc2=s=800x480:r=30:d=$secs" $enc -b:v 4M "$dir/FULL.264"

ffmpeg -loglevel error -y -loop 1 -framerate 30 -i "$tmp/still.png" -t "$secs" \
    -vf "crop=800:480:x='100+t*90':y='80+t*40',scale=400:240,pad=800:480:0:0:black" $enc -b:v 1M "$dir/MARGIN.264"

for s in MAP LIST FULL MARGIN; do
    ffmpeg -loglevel error -y -i "$dir/$s.264" -f rawvideo -pix_fmt yuv420p "$tmp/$s.yuv"
    python3 - "$tmp/$s.yuv" > "$dir/$s.CRC" <<'EOF'
import sys, zlib
frame = 800 * 480 * 3 // 2
data = open(sys.argv[1], 'rb').read()
crc = 0
for i in range(0, len(data), frame):
    crc = zlib.crc32(data[i:i + frame], crc)
    print('%08x' % crc)
EOF
done

here=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "$tmp/card"
cp "$dir"/*.264 "$dir"/*.CRC "$tmp/card/"
[ -n "$3" ] && cp "$3" "$tmp/card/RNSE.BIN"
"$here/target/release/rnse" card "$dir/CARD.IMG" "$tmp/card"
ls -l "$dir"
