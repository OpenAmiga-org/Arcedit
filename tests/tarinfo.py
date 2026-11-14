#!/usr/bin/env python3
"""tarinfo.py ARCHIVE: one line per member: name|mode(octal)|type|comment.
Independent check of arcedit's TAR output (Python's tarfile)."""
import sys, tarfile

with tarfile.open(sys.argv[1]) as t:
    for m in t.getmembers():
        print(f"{m.name}{'/' if m.isdir() else ''}|{m.mode:o}|{m.type.decode()}|{m.pax_headers.get('comment', '')}")
