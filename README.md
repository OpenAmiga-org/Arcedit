# Arcedit

Edit archives without unpacking them: rename, delete and add entries, and
change their comments (file notes) and protection bits. A standalone
command for **AmigaOS 4** and **Linux**, public domain, with no other
libraries needed.
arcedit is an [OpenAmiga.org project](http://openamiga.org/?function=gitproject&name=Arcedit).

Download: [arcedit.lha on OS4Depot](https://os4depot.net/?function=showfile&file=utility/archive/arcedit.lha)
(the AmigaOS 4 command, an Install script, the documentation and icons).

## Layout

- `src/main.c`: Command line and the operations
- `src/arcformat.h`: Interface of an archive format
- `src/lhaformat.c`: LHA backend for that interface
- `src/lha.c`, `src/lha.h`: LHA archive model: headers, reading, writing
- `src/lh5enc.c`: `-lh5-` compressor
- `src/deflate.c`: Deflate compressor and decompressor (RFC 1951), CRC-32
- `src/zipformat.c`, `src/tarformat.c`, `src/gzformat.c`,
  `src/lzxformat.c`: The other format backends
- `src/platform.c`: File system calls (AmigaOS 4 and POSIX for the tests)
- `docs/lha_format.md`: The LHA header details arcedit relies on
- `docs/formats.md`: How each format is handled, and its limits
- `tests/`: Host tests and sample archives made by LhA 2.15
- `tests/os4/`: Scenario tests in an AmigaOS 4 guest (locks, full and write
  protected volumes, low memory, Ctrl-C)
- `package/`: AmigaOS 4 release: readme, Installer script, documentation,
  icons
- `tools/`: `make_lha.sh` (release archive), `make_icons.py` +
  `os4/mkicon.c` (release icons from the OS 4.1 style pictures made in
  `~/os4icons`, see `package/icons/README.md`), `os4depot_submit.py`
  (upload a new version to OS4Depot, replacing the published one)

## Supported formats

| Format | DELETE | RENAME | COMMENT | PROTECT | ADD | New archive | Notes |
|---|---|---|---|---|---|---|---|
| LHA (`.lha`, `.lzh`) | yes | yes | yes | yes | packed (`-lh5-`) | yes | header levels 0-2, self-extracting |
| ZIP (`.zip`) | yes | yes | yes | yes | packed (Deflate) | yes | Info-ZIP Amiga attributes; no ZIP64 |
| TAR (`.tar`) | yes | yes | pax comment | owner r w e | yes | yes | ustar, GNU long names, pax |
| TGZ (`.tgz`, `.tar.gz`) | yes | yes | pax comment | owner r w e | packed | yes | unpacked to a temporary tar and packed again |
| GZIP (`.gz`) | - | stored name | yes | - | replaces the file | yes | one file per `.gz` |
| LZX (`.lzx`) | not part of a merged group | yes | yes | yes | stored | - | headers only |

The formats are the ones xadmaster.library (the AmigaOS archive reading
library) reads; RAR 5 isn't among them. `.bz2` and `.xz` hold one stream
without names or other fields, so there's nothing to edit. Every format is
written in arcedit's own code. Details per format: `docs/formats.md`.

## Using it

```
arcedit ARCHIVE/A,ENTRIES/M,DELETE/S,RENAME/K,AS/K,COMMENT/K,PROTECT/K,ADD/S,TO/K,FROM=LIST/K,INFO/S,WORKDIR/K,QUIET/S
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
| `arcedit Work:a.lha docs DELETE WORKDIR RAM:` | Keep the temporary files in `RAM:` instead of the archive's drawer |

- Entry names are archive paths with `/` between drawers, matched without
  regard to case.
- `COMMENT` and `PROTECT` can be given together, and take a list (`FROM`)
  like `DELETE`.
- `ADD` creates the archive when it doesn't exist yet (`.lha`, `.lzh`,
  `.zip`, `.tar`, `.tgz`, `.tar.gz` or `.gz` name).
  Added entries keep the file's protection bits, file note and date. Empty
  drawers get a drawer entry.
- A rename to a name that's already in the archive is refused.
- `WORKDIR` names an existing drawer for the temporary files (the new
  archive, TGZ's unpacked tar). Free space is then needed there and, for
  the finished archive, in the archive's own drawer. Without it, or with
  `WORKDIR ""`, they go to the archive's drawer. INFO ignores it.
- `QUIET` prints only errors.

Return codes: 0 done, 5 some entries weren't in the archive (the others
were done), 10 nothing was changed (bad arguments, archive error), 20 out
of memory.

## Using arcedit from other programs

- `arcedit <archive> INFO` prints one line: the archive type and the
  operations this archive allows, e.g. `ZIP: DELETE RENAME COMMENT PROTECT
  ADD`. It reads only the archive's first bytes and checks that the archive
  can be replaced (not delete protected, volume not write protected), so
  it is fast for any archive size. An archive that can't be changed gives
  just the type (`LHA:`).
- An operation INFO listed can still fail for reasons only that operation
  shows: a damaged or unsupported archive (ZIP64), not enough free space
  or memory, an added file over 4 GB, one file out of an LZX merged group.
  arcedit checks these before writing anything. Then the last line of the
  output is `Error: <message>` and the return code is 10 (20 for out of
  memory); the archive is unchanged.
- Return code 5 with `Warning:` lines: some entries weren't in the
  archive, the others were done.
- Entry lists for DELETE, COMMENT and PROTECT can be passed in a file
  (`FROM`), one entry per line.

## Names

Entry names inside archives are Latin-1, as on the Amiga. The Linux build
converts UTF-8 names (from the command line, list files and the file
system) to Latin-1 for new names, and a UTF-8 name that matches no entry
is tried again as Latin-1.

## Building

```
make            # Linux (or other POSIX) build: bin/arcedit
make test       # host tests: results checked with Lhasa, unzip, GNU tar,
                # bsdtar, gzip, unar (lsar) and the small readers in
                # tests/*info.py; LHa for UNIX when it's there (LHAUNIX=)
make os4        # AmigaOS 4 build (bin/os4/arcedit) in the walkero/amigagccondocker image
make fuzz       # damaged archives against an AddressSanitizer/UBSan build
python3 tests/corpus.py DIR   # every edit on real archives in DIR, checked with
                # Lhasa, unzip, GNU tar, gzip and unar (the corpus isn't in
                # the repository: download archives from Aminet / OS4Depot)
```

## Design notes

Archives like LHA, ZIP and LZX are lists of members, each a header
followed by that file's own packed data. Every member is packed on its own,
so (LHA as the example):

- **delete** copies the other members byte for byte and leaves the deleted
  ones out,
- **rename**, **comment** and **protection** rebuild only the headers of the
  changed members,
- **add** packs only the new files (`-lh5-`, or stored when that is
  smaller) and replaces entries with the same name.

Nothing is unpacked and repacked (except TGZ, which is one packed stream).
The new archive is written to a temporary file next to the old one
(`aeXXXXXXXX.tmp`, short enough for any file system) and replaces it only
when everything worked; the archive keeps its own protection bits and file
note. Ctrl-C stops arcedit and leaves the archive as it was. `WORKDIR`
puts the temporary files in another drawer; the new archive is then moved
next to the old one (copied when that drawer is on another volume) before
it replaces it, so a failure still leaves the archive as it was.

## Changelog

<!-- changelog start: made by tools/update_readme.py from package/arcedit.readme -->
- **1.1** (2026-11-15):
  - `WORKDIR` keeps the temporary files in another drawer.
  - `PROTECT` on an LHA entry made on Unix removes its Unix permission header, so xadmaster and other readers show the new bits.
- **1.0** (2026-11-15): First release.
<!-- changelog end -->

## History

arcedit was written in November 2026 by Björn Hagström, for the archive
view of Filer (it began as lhaedit, for LHA only). It was developed on
GitHub from the start and moved to the OpenAmiga-org organization in
November 2026.

Public domain: all of arcedit's code is written for it, the Deflate and
`-lh5-` packers too.
