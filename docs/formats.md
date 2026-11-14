# Formats

How arcedit handles each archive type. All code is arcedit's own (written
from the format descriptions), so the project can be public domain.

## LHA

See `lha_format.md`. Everything is supported; added files are packed with
`-lh5-` (stored when that's smaller). Entries made on other systems are
taken as `----rwed`, and PROTECT marks them as Amiga entries.

## ZIP

- Entries are copied as they are (local header, data, data descriptor).
  A rename rebuilds the local header and drops the Info-ZIP Unicode path
  field (it holds the old name); comments and protection live in the
  central directory. Names flagged UTF-8 get the Latin-1 name converted.
- Amiga attributes as Info-ZIP writes them: "version made by" host 1, the
  protection in the upper 16 bits of the external attributes with
  `r w e d` **inverted** (set = allowed), file notes as file comments.
  Checked with unzip and with xadmaster on AmigaOS 4. Unix entries are
  mapped from the owner's rwx; PROTECT makes an entry an Amiga entry.
- Added files are packed with arcedit's Deflate, stored when that's
  smaller. Self-extracting prefixes and the archive comment are kept.
- Not handled: ZIP64 (over 4 GB or 65535 entries), split archives.

## TAR

- ustar headers, GNU long names (`L`) and pax headers (`x`) are read;
  unchanged entries are copied with them.
- Changed entries get a ustar header (name/prefix fields) and, when
  needed, a pax header: `path` for long names, `comment` for the file
  note, plus the pax records that were there.
- PROTECT sets the owner's `r w x` from Amiga `r w e`; `h s p a` have no
  place in tar and aren't stored.

## GZIP and TGZ

- A `.gz` holds one file: RENAME changes the stored name, COMMENT the
  header's comment field, ADD replaces the file (one file only).
- A `.tgz` / `.tar.gz` is one packed stream, so it can't be edited in
  place: arcedit unpacks it into a temporary tar next to the archive, uses
  the TAR backend, and packs the result again.
- `.bz2` and `.xz` are single streams without names or comments; there's
  nothing to edit, so they aren't backends. (`.tar.bz2` / `.tar.xz` would
  work like TGZ but need their own public domain unpackers and packers.)

## RAR

Not handled. arcedit only supports formats that xadmaster.library (the
AmigaOS archive reading library) reads, and xadmaster has no client for
RAR 5, the format current RAR versions write. (A header-editing RAR 5 backend existed
in commits d2e696f..d5b2c7e and was removed.)

## LZX

- Layout from `unlzx.c` 1.1: a 10 byte info header, then per entry a 31
  byte header, name, comment (up to 79 characters) and packed data.
  Header CRC-32 over the header (CRC field 0), name and comment. The date
  is a big-endian bit field (xadmaster agrees; unar reads it the other way).
- Rename, comment and protection change only headers. DELETE works on
  entries that aren't merged and on whole merged groups (files packed
  together); removing part of a group would need repacking and is
  refused. ADD stores files (the LZX packing method is only known from
  unpacking code). New LZX archives aren't created.
- Tested with archives made by LZX 1.21 (a merged packed group, an empty
  drawer entry, SYS:S packed at -3, a stored archive): after arcedit's
  edits LZX itself tests them OK ("all files OK"), and xadmaster and unar
  extract them. LZX reads the dates arcedit writes correctly; LZX 1.21's
  own dates for recent years are off (it writes 1990 for 2026).
- A stored merged group (constructed, `tests/mklzx.py`) is probably never
  written by LZX: xadmaster reports a wrong checksum for its second member
  while unar reads it. arcedit only copies such data.
