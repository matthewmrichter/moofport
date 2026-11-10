#!/usr/bin/env python3
"""mbflags.py - set Finder flags in a MacBinary file, in place, and fix its header CRC.

    scripts/mbflags.py <file.bin> 0x2000

Retro68's Rez writes MacBinary with Finder flags 0. An application with icons (BNDL, FREF,
ICN#) needs hasBundle (0x2000) so the Finder reads its BNDL and shows the icon (Inside Macintosh:
Finder Interface). MacBinary keeps the flags' high byte at offset 73 and the low byte at 101;
the header CRC (offset 124) covers bytes 0-123. Same approach as MoofTCP's scripts/mbflags.py.
"""
import struct
import sys


def crc16x(b):
    c = 0
    for x in b:
        c ^= x << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) & 0xFFFF if c & 0x8000 else (c << 1) & 0xFFFF
    return c


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    path, flags = sys.argv[1], int(sys.argv[2], 0)
    h = bytearray(open(path, "rb").read())
    old = (h[73] << 8) | h[101]
    new = old | flags
    h[73], h[101] = new >> 8, new & 0xFF
    h[124:126] = struct.pack(">H", crc16x(bytes(h[:124])))
    open(path, "wb").write(bytes(h))
    print("%s: Finder flags %04x -> %04x" % (path, old, new))


if __name__ == "__main__":
    main()
