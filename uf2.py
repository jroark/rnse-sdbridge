#!/usr/bin/env python3
"""A UF2 file of a flat RP2040 image (sdbridge.bin, from 0x10000000), for the board's
USB drive (hold BOOT while plugging it in, copy the file onto RPI-RP2).

    python3 sdbridge/uf2.py sdbridge.bin SDBRIDGE.UF2"""
import struct
import sys

FAMILY_RP2040 = 0xE48BFF56
BASE = 0x10000000


def main():
    data = open(sys.argv[1], "rb").read()
    blocks = [data[i:i + 256] for i in range(0, len(data), 256)]
    out = bytearray()
    for n, chunk in enumerate(blocks):
        payload = chunk.ljust(476, b"\0")
        out += struct.pack("<IIIIIIII", 0x0A324655, 0x9E5D5157, 0x2000, BASE + 256 * n, 256, n, len(blocks), FAMILY_RP2040)
        out += payload + struct.pack("<I", 0x0AB16F30)
    open(sys.argv[2], "wb").write(out)


if __name__ == "__main__":
    main()
