# Release icons

These `.info` files were saved by AmigaOS 4's icon.library (native format,
64x64 true colour pictures, normal and selected). AmigaOS 4 does not read
PNG icons, so they can't be written on the host.

Their pictures are in `pictures/`: `<icon>.png` (normal) and
`<icon>.sel.png` (selected), 64x64 RGBA, in the OS 4.1 style. The drawers
use the stock `def_drawer` picture as their base, as most Amiga programs
do. The selected pictures are made like the stock OS 4.1 ones (each
colour channel 84 darker, gold glow), so they are used as they are.

| Icon | Picture | In the archive |
|------|---------|----------------|
| `arcedit` | drawer with a box and a pencil | `arcedit.info` |
| `Install` | drive with a green arrow | `arcedit/Install.info` |
| `arcedit.txt` | text page | `arcedit/arcedit.txt.info` |
| `C` | drawer with a shell window | `arcedit/C.info` |
| `C_arcedit` | shell window with a box and a pencil | `arcedit/C/arcedit.info` |

To change an icon:

1. Change its pictures in `pictures/`.
2. `python3 -I tools/make_icons.py build/icons` writes `*.argb`,
   `mkicons.script` and `icons-preview.png` (a second argument takes the
   pictures from another drawer; the icons' types, tools, tool types,
   stacks and windows are in its ICONS table).
3. Build `tools/os4/mkicon.c` with `ppc-amigaos-gcc` and copy it, the
   `.argb` files and the script to a drawer in the OS4 guest.
4. Run `Execute mkicons.script` in that drawer; the icons appear in `out/`.
5. Copy them back here. `C_arcedit.info` becomes `C/arcedit.info` in the
   archive (tools/make_lha.sh does this).
