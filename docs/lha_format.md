# LHA headers as arcedit reads and writes them

Checked against archives made by LhA 2.15 AOS4 (`tests/samples/`, files with
a file note, unusual protection bits and a subdrawer, made with `-H0`,
`-H1`, `-H2` and with `-e` for drawer entries). LhA's default is level 1.

All numbers are little endian. An archive is a list of members (header +
compressed data) ended by a single `0x00` byte. Self-extracting archives
have code in front of the first header.

## Common fields (offsets 2..20)

| Offset | Size | Field |
|---|---|---|
| 2 | 5 | method, `-lh5-`, `-lh0-` (stored) ... |
| 7 | 4 | level 0/2: compressed size; level 1: compressed size + extended headers |
| 11 | 4 | original size |
| 15 | 4 | level 0/1: MS-DOS date/time (local); level 2: seconds since 1970, local time written as if UTC |
| 19 | 1 | attribute: LhA puts the low byte of the Amiga protection bits here |
| 20 | 1 | header level |

Protection byte: bits 0-3 `d e w r` are set when the action is **not**
allowed, bits 4-7 `a p s h` are set when the flag is on (as in
`fib_Protection`).

## Level 0

| Offset | Field |
|---|---|
| 0 | header size - 2 (from offset 2 to the end of the header) |
| 1 | checksum: sum of the bytes from offset 2 to the end, low byte |
| 21 | name field length |
| 22 | name field: `path\name`, then optionally `\0` and the file note |
| 22+n | CRC-16 of the data |

## Level 1

As level 0 up to the data CRC, but the name field holds only the name
(+ `\0` + file note), followed by the OS id (`A`) and the size of the first
extended header. The extended headers follow the base header and count in
the size at offset 7. The checksum covers only the base header.

Each extended header is `type, data, size of the next extended header (2)`.

| Type | Contents |
|---|---|
| `0x00` | CRC-16 of the whole header (base + extended headers, with this field 0); LhA writes it only when there are other extended headers |
| `0x02` | drawer path, `0xFF` between parts and at the end |

A drawer entry (`-e`) has an empty name, `-lh0-`, size 0 and the drawer in
`0x02`.

## Level 2

| Offset | Field |
|---|---|
| 0 | total header size (2 bytes) |
| 19 | the protection byte again |
| 21 | CRC-16 of the data |
| 23 | OS id |
| 24 | size of the first extended header |

Extended headers, in LhA's order: `0x01` name, `0x02` drawer, `0x71` file
note, `0x40` attributes (2 bytes, the protection in the first), `0x00`
header CRC (over the whole header, field 0). The total size's low byte must
not be 0 (it would read as the end marker); a padding byte is added then.

## Archives from other systems

Other archivers (LHa for UNIX, MS-DOS LHA) write MS-DOS attributes into
the attribute byte (LHa for UNIX: `0x20`) and their own permissions in
extended headers (`0x50` Unix mode, `0x51` owner, `0x54` time). LhA 2.15
ignores the attribute byte of such entries and shows `----rwed`.
arcedit therefore treats their protection as `----rwed`, and when PROTECT
changes it, marks the entry as Amiga (OS `A`) so LhA uses the new bits.
The other system's extended headers are kept.

## CRC

CRC-16 with polynomial `0xA001` (reflected), start value 0, for both data
and headers.

## What arcedit does

- Unchanged members: header and data copied byte for byte.
- Changed members: header rebuilt in its level, the data copied. Names that
  don't fit a level 0/1 name field move that member to level 2 (time
  converted).
- Added members: the level of the archive's first member (1 for a new
  archive), `-lh5-` or `-lh0-`, OS `A`.
