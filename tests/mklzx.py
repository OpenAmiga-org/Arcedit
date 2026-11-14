#!/usr/bin/env python3
"""mklzx.py OUT: write a small LZX archive (stored entries and a merged
stored group) following the header layout of unlzx.c, as a test sample."""
import struct, sys, zlib


def date(y, mo, d, h, mi, s):
    return (d << 27) | ((mo - 1) << 23) | ((y - 1970) << 17) | (h << 12) | (mi << 6) | s


def entry(name, data, prot, comment=b"", merged=False, packsize=None):
    h = bytearray(31)
    h[0] = prot
    struct.pack_into("<I", h, 2, len(data))
    struct.pack_into("<I", h, 6, len(data) if packsize is None else packsize)
    h[10], h[11], h[12], h[14], h[15], h[30] = 10, 0, 1 if merged else 0, len(comment), 10, len(name)
    struct.pack_into(">I", h, 18, date(2026, 11, 14, 22, 5, 44))
    struct.pack_into("<I", h, 22, zlib.crc32(data))
    struct.pack_into("<I", h, 26, zlib.crc32(bytes(h) + name + comment))
    return bytes(h) + name + comment


a = b"hello world, hello world\n"
b = b"second file\n"
c1, c2 = b"merged one\n", b"merged two!\n"
out = bytearray(b"LZX\x00\x00\x00\x00\x00\x00\x00")
out += entry(b"a.txt", a, 0x4F | 0x40 & 0, b"note for a") + a           # rwed + s? keep simple
out += entry(b"sub/b.txt", b, 0x0D) + b                                # r-ed (no w)
out += entry(b"m/one.txt", c1, 0x0F, merged=True, packsize=0)          # merged group: data after the last
out += entry(b"m/two.txt", c2, 0x0F, merged=True, packsize=len(c1) + len(c2)) + c1 + c2
open(sys.argv[1], "wb").write(out)
