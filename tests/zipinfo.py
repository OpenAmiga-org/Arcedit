#!/usr/bin/env python3
"""zipinfo.py ARCHIVE: one line per entry: name|host|prot|method|comment.
Independent check of arcedit's ZIP output (Python's zipfile reader); also
tests every entry's CRC (exit 1 on a bad one)."""
import sys, zipfile


def prot(attr):
    s = ""
    for i, ch in enumerate("hspa"):
        s += ch if attr & (0x80 >> i) else "-"
    for i, ch in enumerate("rwed"):
        s += "-" if attr & (0x08 >> i) else ch
    return s


with zipfile.ZipFile(sys.argv[1]) as z:
    bad = z.testzip()
    for i in z.infolist():
        # Info-ZIP stores the Amiga rwed bits inverted (set = allowed)
        p = prot(((i.external_attr >> 16) & 0xFF) ^ 0x0F) if i.create_system == 1 else oct(i.external_attr >> 16)
        print(f"{i.filename}|{i.create_system}|{p}|{i.compress_type}|{i.comment.decode('latin-1')}")
    if z.comment:
        print("#" + z.comment.decode("latin-1"))
    sys.exit(1 if bad else 0)
