#!/usr/bin/env python3
"""LINKTEST.BIN for sdk/examples/linkbench: 32-bit big-endian words, word j being
pattern(j) below, so that the program can tell every word it reads from the card.

    python3 tools/linktest.py [MEGABYTES] > /path/to/card/LINKTEST.BIN

2 MB by default, which is as much as the program reads.  Copy it onto a card that has
just been formatted, so that the file lies in one run of clusters."""
import struct
import sys


def pattern(j):
    x = ((j + 1) * 0x9E3779B1) & 0xFFFFFFFF
    x ^= x >> 16
    x = (x * 0x85EBCA6B) & 0xFFFFFFFF
    x ^= x >> 13
    return x


def main():
    mb = int(sys.argv[1]) if len(sys.argv) > 1 else 2
    out = sys.stdout.buffer
    words = mb << 18
    for start in range(0, words, 1 << 16):
        out.write(struct.pack(">%dI" % (1 << 16), *(pattern(j) for j in range(start, start + (1 << 16)))))


if __name__ == "__main__":
    main()
