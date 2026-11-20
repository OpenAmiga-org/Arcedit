# Release icons

These `.info` files were saved by AmigaOS 4's icon.library (native format,
64x64 true colour pictures, normal and selected). AmigaOS 4 does not read
PNG icons, so they can't be written on the host.

The pictures are the OS 4.1 style set made with the icon tools in
`~/os4icons` (export set `arcedit`: `out/arcedit/<icon>.png` and
`<icon>.sel.png`; the drawers use the stock `def_drawer` picture as their
base, as most Amiga programs do). To change an icon:

1. Make or change the pictures in `~/os4icons` and export them.
2. `python3 -I tools/make_icons.py build/icons [PICTURES]` writes `*.argb`,
   `mkicons.script` and `icons-preview.png` (PICTURES defaults to
   `~/os4icons/out/arcedit`; the icons' types, tools, tool types, stacks
   and windows are in its ICONS table).
3. Build `tools/os4/mkicon.c` with `ppc-amigaos-gcc` and copy it, the
   `.argb` files and the script to a drawer in the OS4 guest.
4. Run `Execute mkicons.script` in that drawer; the icons appear in `out/`.
5. Copy them back here. `C_arcedit.info` becomes `C/arcedit.info` in the
   archive (tools/make_lha.sh does this).
