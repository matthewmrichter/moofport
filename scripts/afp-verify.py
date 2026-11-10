#!/usr/bin/env python3
"""afp-verify.py - check AfpCopy's server copy from the host side.

    scripts/afp-verify.py <path to "AfpCopy test" on the server> [kb]

AfpCopy (mac/afpcopy) writes a reproducible pattern; this regenerates the data fork (xorshift32,
seed 'Moof', big-endian words, `kb` KB, default 320) and compares the file on the server
(netatalk keeps the data fork as the plain file). Prints the CRC-32, which AfpCopy also logs.
"""
import sys
import zlib


def pattern(n, seed=0x4D6F6F66):
    s, out = seed, bytearray()
    while len(out) < n:
        s ^= (s << 13) & 0xFFFFFFFF
        s ^= s >> 17
        s ^= (s << 5) & 0xFFFFFFFF
        out += s.to_bytes(4, "big")
    return bytes(out[:n])


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    n = (int(sys.argv[2]) if len(sys.argv) > 2 else 320) * 1024
    got, want = open(sys.argv[1], "rb").read(), pattern(n)
    print("server file %d bytes, CRC-32 %08x; expected %d bytes, CRC-32 %08x"
          % (len(got), zlib.crc32(got), n, zlib.crc32(want)))
    if got != want:
        bad = next((i for i in range(min(len(got), n)) if got[i] != want[i]), min(len(got), n))
        sys.exit("DIFFERENT from offset %d" % bad)
    print("identical")


if __name__ == "__main__":
    main()
