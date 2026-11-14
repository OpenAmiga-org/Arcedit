#!/usr/bin/env python3
"""lzxinfo.py ARCHIVE: name|prot(hsparwed)|merged|comment per entry,
checking header CRCs (independent of arcedit's reader)."""
import struct, sys, zlib

d = open(sys.argv[1], "rb").read()
assert d[:3] == b"LZX"
p = 10
while p + 31 <= len(d):
    h = d[p:p + 31]
    nl, cl = h[30], h[14]
    name, comment = d[p + 31:p + 31 + nl], d[p + 31 + nl:p + 31 + nl + cl]
    hz = bytearray(h); hz[26:30] = b"\0\0\0\0"
    assert zlib.crc32(bytes(hz) + name + comment) == struct.unpack_from("<I", h, 26)[0], "header crc"
    a = h[0]
    prot = "".join(c if a & b else "-" for c, b in zip("hspa", (32, 64, 128, 16))) + \
           "".join(c if a & b else "-" for c, b in zip("rwed", (1, 2, 8, 4)))
    print(f"{name.decode('latin-1')}|{prot}|{h[12] & 1}|{comment.decode('latin-1')}")
    p += 31 + nl + cl + struct.unpack_from("<I", h, 6)[0]
