# arcedit

Edit archives without unpacking them. An AmigaOS 4 command that renames,
deletes, adds and changes the comment and protection bits of archive
entries.

Every format is written in arcedit's own code, so the whole project can be
released as public domain.

| Format | DELETE | RENAME | COMMENT | PROTECT | ADD | New archive | Notes |
|---|---|---|---|---|---|---|---|
| LHA (`.lha`, `.lzh`) | yes | yes | yes | yes | packed (`-lh5-`) | yes | header levels 0-2, self-extracting |
| ZIP (`.zip`) | yes | yes | yes | yes | packed (Deflate) | yes | Info-ZIP Amiga attributes; no ZIP64 |
| TAR (`.tar`) | yes | yes | pax comment | owner r w e | yes | yes | ustar, GNU long names, pax |
| TGZ (`.tgz`, `.tar.gz`) | yes | yes | pax comment | owner r w e | packed | yes | unpacked to a temporary tar and packed again |
| GZIP (`.gz`) | - | stored name | yes | - | replaces the file | yes | one file per `.gz` |
| RAR 5 (`.rar`) | not solid | yes | - | Unix/Windows | stored | - | headers only; no RAR 4 |
| LZX (`.lzx`) | not part of a merged group | yes | yes | yes | stored | - | headers only |

`arcedit <archive> INFO` prints what the archive allows, for example
`ZIP: DELETE RENAME COMMENT PROTECT ADD`; an operation the archive doesn't
allow is refused before anything is written. `.bz2` and `.xz` hold one
stream without names or other fields, so there's nothing for arcedit to
edit there. Details per format: `docs/formats.md`.

## How it works

Archives like LHA, ZIP, RAR and LZX are lists of members, each a header
followed by that file's own packed data. Every member is packed on its own,
so (LHA as the example):

- **delete** copies the other members byte for byte and leaves the deleted
  ones out,
- **rename**, **comment** and **protection** rebuild only the headers of the
  changed members,
- **add** packs only the new files (`-lh5-`, or stored when that is
  smaller) and replaces entries with the same name.

Nothing is unpacked and repacked. The new archive is written next to the
old one (`<archive>.arcedit`) and replaces it only when everything worked;
the archive keeps its own protection bits and file note.

## Usage

```
arcedit ARCHIVE/A,ENTRIES/M,DELETE/S,RENAME/K,AS/K,COMMENT/K,PROTECT/K,ADD/S,TO/K,FROM=LIST/K,INFO/S,QUIET/S
```

| Command | Does |
|---|---|
| `arcedit Work:a.lha docs readme.txt DELETE` | Delete entries; a drawer name deletes everything in it |
| `arcedit Work:a.lha DELETE FROM RAM:list` | Delete the entries listed in a file, one per line |
| `arcedit Work:a.lha RENAME docs/a.txt AS docs/b.txt` | Rename (or move) a file |
| `arcedit Work:a.lha RENAME docs AS Documentation` | Rename a drawer with everything in it |
| `arcedit Work:a.lha docs/a.txt COMMENT "text"` | Set the file note (`""` clears it) |
| `arcedit Work:a.lha docs/a.txt PROTECT rwed` | Set the protection bits exactly |
| `arcedit Work:a.lha docs/a.txt PROTECT +s-d` | Add or remove protection bits (`hsparwed`) |
| `arcedit Work:a.lha ADD RAM:x RAM:Drawer TO docs` | Add files and drawers (with their contents) below `docs` |
| `arcedit Work:a.zip INFO` | Show the format and the operations it allows |

- Entry names are archive paths with `/` between drawers, matched without
  regard to case.
- `COMMENT` and `PROTECT` can be given together, and take a list (`FROM`)
  like `DELETE`.
- `ADD` creates the archive when it doesn't exist yet (`.lha`/`.lzh` name).
  Added entries keep the file's protection bits, file note and date. Empty
  drawers get a drawer entry.
- A rename to a name that's already in the archive is refused.
- `QUIET` prints only errors.

Return codes: 0 done, 5 some entries weren't in the archive (the others
were done), 10 nothing was changed (bad arguments, archive error), 20 out
of memory.

## Building

```
make            # host build (bin/arcedit), used by the tests
make test       # host tests: results checked with Lhasa, unzip, GNU tar,
                # bsdtar, gzip, unrar, unar (lsar) and the small readers in
                # tests/*info.py; LHa for UNIX when it's there (LHAUNIX=)
make os4        # AmigaOS 4 build (bin/os4/arcedit) in the walkero/amigagccondocker image
```

## Files

| File | Contents |
|---|---|
| `src/main.c` | Command line and the operations |
| `src/arcformat.h` | Interface of an archive format |
| `src/lhaformat.c` | LHA backend for that interface |
| `src/lha.c`, `src/lha.h` | LHA archive model: headers, reading, writing |
| `src/lh5enc.c` | `-lh5-` compressor |
| `src/deflate.c` | Deflate compressor and decompressor (RFC 1951), CRC-32 |
| `src/zipformat.c`, `src/tarformat.c`, `src/gzformat.c`, `src/rarformat.c`, `src/lzxformat.c` | The other format backends |
| `src/platform.c` | File system calls (AmigaOS 4 and POSIX for the tests) |
| `docs/lha_format.md` | The LHA header details arcedit relies on |
| `docs/formats.md` | How each format is handled, and its limits |
| `tests/` | Host tests and sample archives made by LhA 2.15 |
