#!/usr/bin/env python3
"""gzinfo.py FILE: name|comment of a gzip file's first header (independent
check of arcedit's gzip output)."""
import sys

d = open(sys.argv[1], "rb").read()
assert d[:3] == b"\x1f\x8b\x08", "not gzip"
fl, p = d[3], 10
if fl & 4:
    p += 2 + d[p] + (d[p + 1] << 8)
name = comment = b""
if fl & 8:
    e = d.index(b"\0", p); name = d[p:e]; p = e + 1
if fl & 16:
    e = d.index(b"\0", p); comment = d[p:e]; p = e + 1
print(f"{name.decode('latin-1')}|{comment.decode('latin-1')}")
