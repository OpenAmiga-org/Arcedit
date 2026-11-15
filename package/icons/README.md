# Release icons

These `.info` files were saved by AmigaOS 4's icon.library (native format,
64x64 true colour picture). AmigaOS 4 does not read PNG icons, so they
can't be written on the host.

The pictures are drawn by `tools/make_icons.py` (public domain, like the
rest of arcedit). To change an icon:

1. `python3 -I tools/make_icons.py build/icons` writes `*.argb`,
   `mkicons.script` and `icons-preview.png`.
2. Build `tools/os4/mkicon.c` with `ppc-amigaos-gcc` and copy it, the
   `.argb` files and the script to a drawer in the OS4 guest.
3. Run `Execute mkicons.script` in that drawer; the icons appear in `out/`.
4. Copy them back here. `C_arcedit.info` becomes `C/arcedit.info` in the
   archive (tools/make_lha.sh does this).
