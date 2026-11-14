#!/usr/bin/env python3
"""lhainfo.py ARCHIVE: print one line per member: path, level, method,
protection (Amiga hsparwed), comment. Independent of arcedit's own reader,
used by the tests to check header fields."""
import struct
import sys


def prot(attr):
    s = ""
    for i, ch in enumerate("hspa"):
        s += ch if attr & (0x80 >> i) else "-"
    for i, ch in enumerate("rwed"):
        s += "-" if attr & (0x08 >> i) else ch
    return s


def members(data):
    off = 0
    # self-extracting archives: skip to the first header
    while off + 22 <= len(data) and not (data[off + 2:off + 4] == b"-l" and data[off + 6:off + 7] == b"-"):
        off += 1
    while off < len(data) and data[off] != 0:
        level = data[off + 20]
        method = data[off + 2:off + 7].decode()
        attr = data[off + 19]
        comment = ""
        dirname = ""
        if level in (0, 1):
            hs = data[off] + 2
            assert sum(data[off + 2:off + hs]) & 0xFF == data[off + 1], "header checksum"
            nl = data[off + 21]
            field = data[off + 22:off + 22 + nl]
            name, _, comment = field.partition(b"\0")
            name = name.decode("latin-1").replace("\\", "/")
            comment = comment.decode("latin-1")
            pos = off + hs
            size = struct.unpack_from("<I", data, off + 7)[0]
            if level == 1:
                nxt = struct.unpack_from("<H", data, off + hs - 2)[0]
                extsize = 0
                while nxt:
                    t = data[pos]
                    d = data[pos + 1:pos + nxt - 2]
                    if t == 2:
                        dirname = d.replace(b"\xff", b"/").decode("latin-1")
                    elif t == 1:
                        name = d.decode("latin-1")
                    extsize += nxt
                    pos += nxt
                    nxt = struct.unpack_from("<H", data, pos - 2)[0]
                size -= extsize
        else:
            hs = struct.unpack_from("<H", data, off)[0]
            size = struct.unpack_from("<I", data, off + 7)[0]
            pos = off + 26
            nxt = struct.unpack_from("<H", data, off + 24)[0]
            name = ""
            while nxt:
                t = data[pos]
                d = data[pos + 1:pos + nxt - 2]
                if t == 1:
                    name = d.decode("latin-1")
                elif t == 2:
                    dirname = d.replace(b"\xff", b"/").decode("latin-1")
                elif t == 0x71:
                    comment = d.decode("latin-1")
                elif t == 0x40:
                    attr = d[0]
                pos += nxt
                nxt = struct.unpack_from("<H", data, pos - 2)[0]
            pos = off + hs
        yield dirname + name, level, method, prot(attr), comment
        off = pos + size


if __name__ == "__main__":
    for path, level, method, p, comment in members(open(sys.argv[1], "rb").read()):
        print(f"{path}|{level}|{method}|{p}|{comment}")
