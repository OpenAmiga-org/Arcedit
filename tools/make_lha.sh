#!/bin/bash
# Puts the AmigaOS 4 release together and packs it: dist/arcedit.lha
#
#   make os4 && tools/make_lha.sh
#
# Layout:
#   arcedit/              drawer (+ icon)
#     Install             Installation Utility script
#     arcedit.txt         documentation
#     C/arcedit           the command
#   arcedit.readme        OS4Depot readme
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LHA="${LHA:-$HOME/linux_archivers/lha/lha}"
STAGE="$ROOT/build/stage"
OUT="$ROOT/dist/arcedit.lha"
BIN="$ROOT/bin/os4/arcedit"

vs=$(sed -n 's/^#define VERSION_STRING "arcedit \(.*\)"/\1/p' "$ROOT/src/main.c")   # 1.0 (15.11.2026)
ver=${vs%% *}
amigadate=$(echo "$vs" | sed 's/.*(\(.*\))/\1/')
isodate=$(echo "$amigadate" | awk -F. '{printf "%04d-%02d-%02d", $3, $2, $1}')

if ! strings "$BIN" | grep -q "\$VER: arcedit $vs"; then
	echo "bin/os4/arcedit is not arcedit $vs; run make os4" >&2
	exit 1
fi

rm -rf "$STAGE"
D="$STAGE/arcedit"
mkdir -p "$D/C" "$ROOT/dist"
cp "$BIN" "$D/C/arcedit"
# without the symbols (the build keeps them for debugging)
sudo docker run --rm -u "$(id -u):$(id -g)" -v "$D/C:/c" walkero/amigagccondocker:os4-gcc15-3.6.1 \
	ppc-amigaos-strip /c/arcedit
cp "$ROOT/package/arcedit/Install" "$D/"
sed -e "s/@VERSION@/$ver/g" -e "s/@AMIGADATE@/$amigadate/g" "$ROOT/package/arcedit/arcedit.txt" > "$D/arcedit.txt"
sed -e "s/@VERSION@/$ver/g" -e "s/@DATE@/$isodate/g" "$ROOT/package/arcedit.readme" > "$STAGE/arcedit.readme"

# icons saved by icon.library itself (tools/make_icons.py, package/icons/README.md)
I="$ROOT/package/icons"
cp "$I/arcedit.info" "$STAGE/"
cp "$I/Install.info" "$I/arcedit.txt.info" "$I/C.info" "$D/"
cp "$I/C_arcedit.info" "$D/C/arcedit.info"

rm -f "$OUT"
(cd "$STAGE" && "$LHA" aq2o5 "$OUT" arcedit.info arcedit.readme arcedit)
"$LHA" l "$OUT"
echo "arcedit $ver ($amigadate) -> $OUT"
