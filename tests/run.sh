#!/bin/sh
# Host tests for arcedit. Every result is checked with LHa for UNIX
# ("lha t" tests all CRCs, "lha x" extracts for byte compares) and with
# tests/lhainfo.py for the header fields (comment, protection, level).
#
#   sh tests/run.sh        (from the project root, after "make")

cd "$(dirname "$0")/.." || exit 1
AE=$PWD/bin/arcedit
INFO="python3 $PWD/tests/lhainfo.py"
W=$PWD/tests/work
pass=0
fail=0

ok()   { pass=$((pass + 1)); }
bad()  { fail=$((fail + 1)); echo "FAIL: $*"; }
check() { if eval "$2"; then ok; else bad "$1"; fi; }

# lha t must pass and print no errors
lhatest() { lha t "$1" >/dev/null 2>"$W/lhat.err" && ! grep -qi 'error\|crc' "$W/lhat.err"; }

reset() {
 [ -n "$W" ] && [ -d "$W" ] && find "$W" -mindepth 1 -delete
 mkdir -p "$W"
}

reset
for lvl in h0 h1 h2 hd; do
 S=tests/samples/$lvl.lha

 # comment
 cp $S $W/a.lha
 $AE $W/a.lha a.txt COMMENT "new comment" QUIET
 check "$lvl comment rc" "[ \$? -eq 0 ]"
 check "$lvl comment lha t" "lhatest $W/a.lha"
 check "$lvl comment value" "$INFO $W/a.lha | grep -q '^a.txt|.*|new comment\$'"
 check "$lvl comment others kept" "$INFO $W/a.lha | grep -q '^sub/b.txt|'"

 # clear comment
 $AE $W/a.lha a.txt COMMENT "" QUIET
 check "$lvl comment clear" "$INFO $W/a.lha | grep -q '^a.txt|.*|\$'"

 # protection: exact, + and -
 cp $S $W/a.lha
 $AE $W/a.lha sub/b.txt PROTECT rwd QUIET
 check "$lvl protect exact" "$INFO $W/a.lha | grep -q '^sub/b.txt|.*|----rw-d|'"
 $AE $W/a.lha sub/b.txt PROTECT +sp-w QUIET
 check "$lvl protect +-" "$INFO $W/a.lha | grep -q '^sub/b.txt|.*|-sp-r--d|'"
 check "$lvl protect lha t" "lhatest $W/a.lha"
 $AE $W/a.lha sub/b.txt PROTECT +x QUIET 2>/dev/null
 check "$lvl protect bad letter" "[ \$? -eq 10 ]"

 # rename file and drawer, data unchanged
 cp $S $W/a.lha
 $AE $W/a.lha RENAME a.txt AS docs/first.txt QUIET
 check "$lvl rename file" "$INFO $W/a.lha | grep -q '^docs/first.txt|.*|note for a\$'"
 $AE $W/a.lha RENAME sub AS other QUIET
 check "$lvl rename drawer" "$INFO $W/a.lha | grep -q '^other/b.txt|'"
 check "$lvl rename lha t" "lhatest $W/a.lha"
 mkdir -p $W/x && (cd $W/x && lha xq ../a.lha >/dev/null 2>&1)
 check "$lvl rename data" "cmp -s $W/x/other/b.txt $W/x/other/b.txt && grep -q 'second file' $W/x/other/b.txt && grep -q 'hello world' $W/x/docs/first.txt"
 rm -rf $W/x
 $AE $W/a.lha RENAME docs/first.txt AS other/b.txt QUIET
 check "$lvl rename onto existing refused" "[ \$? -eq 10 ]"
 $AE $W/a.lha RENAME nothing AS other QUIET
 check "$lvl rename missing" "[ \$? -eq 10 ]"

 # delete a drawer and a file; missing name warns
 cp $S $W/a.lha
 $AE $W/a.lha sub DELETE QUIET
 check "$lvl delete drawer" "! $INFO $W/a.lha | grep -q '^sub/'"
 check "$lvl delete keeps" "$INFO $W/a.lha | grep -q '^a.txt|'"
 check "$lvl delete lha t" "lhatest $W/a.lha"
 $AE $W/a.lha nosuch DELETE QUIET
 check "$lvl delete missing warns" "[ \$? -eq 5 ]"

 # delete from a list file
 cp $S $W/a.lha
 printf 'a.txt\nsub/b.txt\n' > $W/list
 $AE $W/a.lha DELETE FROM $W/list QUIET
 check "$lvl delete list" "[ \$($INFO $W/a.lha | grep -c '\\.txt') -eq 0 ]"
done

# ADD: compressor round trips (new archive, level 1)
reset
mkdir -p $W/in/deep/er $W/in/empty
head -c 200000 /dev/urandom > $W/in/random.bin
yes "the quick brown fox jumps over the lazy dog" | head -c 300000 > $W/in/text.txt
head -c 100000 /dev/zero > $W/in/zeros.bin
cp /usr/bin/ls $W/in/deep/binary
: > $W/in/emptyfile
printf 'x' > $W/in/deep/er/one
cat src/*.c > $W/in/deep/er/source.c
$AE $W/new.lha ADD $W/in QUIET
check "add rc" "[ \$? -eq 0 ]"
check "add lha t" "lhatest $W/new.lha"
mkdir -p $W/out && (cd $W/out && lha xq ../new.lha >/dev/null 2>&1)
for f in random.bin text.txt zeros.bin deep/binary emptyfile deep/er/one deep/er/source.c; do
 check "add data $f" "cmp -s $W/in/$f $W/out/in/$f"
done
check "add empty drawer entry" "$INFO $W/new.lha | grep -q '^in/empty/|'"
check "add compresses text" "lha v $W/new.lha 2>/dev/null | awk '/in\/text.txt/ { exit !(\$3 < 5000) }'"
check "add random stored" "$INFO $W/new.lha | grep -q '^in/random.bin|1|-lh0-|'"

# ADD into an existing archive, below a drawer, replacing an entry
for lvl in h0 h1 h2; do
 cp tests/samples/$lvl.lha $W/a.lha
 $AE $W/a.lha ADD $W/in/deep/er/source.c TO sub QUIET
 check "$lvl add to drawer" "$INFO $W/a.lha | grep -q '^sub/source.c|'"
 check "$lvl add lha t" "lhatest $W/a.lha"
 printf 'replaced\n' > $W/b.txt
 $AE $W/a.lha ADD $W/b.txt TO sub QUIET
 check "$lvl add replaces" "[ \$($INFO $W/a.lha | grep -c '^sub/b.txt|') -eq 1 ]"
 rm -rf $W/out2 && mkdir -p $W/out2 && (cd $W/out2 && lha xq ../a.lha >/dev/null 2>&1)
 check "$lvl add replaced data" "cmp -s $W/b.txt $W/out2/sub/b.txt"
 check "$lvl add source data" "cmp -s $W/in/deep/er/source.c $W/out2/sub/source.c"
done

# a long name moves a level 0/1 header to level 2
cp tests/samples/h1.lha $W/a.lha
long=$(printf 'n%.0s' $(seq 1 240))
$AE $W/a.lha RENAME a.txt AS $long QUIET
check "long name level 2" "$INFO $W/a.lha | grep -q '^$long|2|'"
check "long name lha t" "lhatest $W/a.lha"

# not an archive / several operations
printf 'hello' > $W/plain.txt
$AE $W/plain.txt x DELETE QUIET >/dev/null
check "not an archive" "[ \$? -eq 10 ]"
$AE $W/a.lha x DELETE COMMENT y QUIET >/dev/null
check "two operations refused" "[ \$? -eq 10 ]"

# Archives made by LHa for UNIX (level 2, Unix extended headers), when that
# tool is there: edits keep its headers, packing is close to its own
LHAUNIX=${LHAUNIX:-/home/claude/linux_archivers/lha/lha}
if [ -x "$LHAUNIX" ]; then
 (cd $W && "$LHAUNIX" -ao5q u.lha in/text.txt in/deep/er/source.c in/deep/er/one in/emptyfile)
 cp $W/u.lha $W/e.lha
 $AE $W/e.lha in/text.txt COMMENT "unix note" QUIET
 $AE $W/e.lha RENAME in/deep AS in/Deep QUIET
 $AE $W/e.lha in/emptyfile DELETE QUIET
 $AE $W/e.lha ADD src/lha.h TO in QUIET
 check "unix: lha -t" "\"$LHAUNIX\" -t $W/e.lha >/dev/null 2>&1"
 check "unix: comment" "$INFO $W/e.lha | grep -q '^in/text.txt|.*|unix note\$'"
 check "unix: unix headers kept" "\"$LHAUNIX\" -v $W/e.lha | grep -q '1000/1000.*in/Deep/er/source.c'"
 rm -rf $W/ux && mkdir -p $W/ux && (cd $W/ux && "$LHAUNIX" -xq ../e.lha)
 check "unix: data" "cmp -s $W/in/text.txt $W/ux/in/text.txt && cmp -s $W/in/deep/er/source.c $W/ux/in/Deep/er/source.c && cmp -s src/lha.h $W/ux/in/lha.h"
 # protection of a non-Amiga entry: starts from ----rwed, entry becomes Amiga
 $AE $W/e.lha in/Deep/er/one PROTECT +s QUIET
 check "unix: protect" "$INFO $W/e.lha | grep -q '^in/Deep/er/one|2|-lh0-|-s--rwed|'"
 # packing within 1% of LHa for UNIX -o5 on the same files
 (cd $W && "$LHAUNIX" -ao5q ref.lha in/deep/er/source.c && $AE ae.lha ADD in/deep/er/source.c QUIET)
 check "unix: packing close to lha" "[ \$(stat -c %s $W/ae.lha) -le \$(( \$(stat -c %s $W/ref.lha) * 101 / 100 + 64 )) ]"
else
 echo "(LHa for UNIX not found at $LHAUNIX, its tests skipped)"
fi

# ---------------------------------------------------------------- ZIP
ZI="python3 $PWD/tests/zipinfo.py"
ziptest() { unzip -tq "$1" >/dev/null 2>&1 && $ZI "$1" >/dev/null 2>&1; }
for z in z_unix z_stream z_amiga; do
 S=tests/samples/$z.zip
 cp $S $W/a.zip
 $AE $W/a.zip a.txt COMMENT "zip note" QUIET
 check "$z comment" "$ZI $W/a.zip | grep -q '^a.txt|.*|zip note\$'"
 check "$z comment unzip -t" "ziptest $W/a.zip"
 $AE $W/a.zip sub/b.txt PROTECT +s-w QUIET
 check "$z protect" "$ZI $W/a.zip | grep -q '^sub/b.txt|1|-s--r-[e-][d-]|'"
 cp $S $W/a.zip
 $AE $W/a.zip RENAME a.txt AS docs/first.txt QUIET
 $AE $W/a.zip RENAME sub AS other QUIET
 check "$z rename" "$ZI $W/a.zip | grep -q '^docs/first.txt|' && $ZI $W/a.zip | grep -q '^other/b.txt|' && ! $ZI $W/a.zip | grep -q '^sub/'"
 check "$z rename unzip -t" "ziptest $W/a.zip"
 rm -rf $W/zx && mkdir -p $W/zx && (cd $W/zx && unzip -q ../a.zip)
 check "$z rename data" "grep -q 'hello world' $W/zx/docs/first.txt && grep -q 'second file' $W/zx/other/b.txt"
 cp $S $W/a.zip
 $AE $W/a.zip sub DELETE QUIET
 check "$z delete drawer" "! $ZI $W/a.zip | grep -q '^sub' && $ZI $W/a.zip | grep -q '^a.txt|'"
 check "$z delete unzip -t" "ziptest $W/a.zip"
 $AE $W/a.zip ADD $W/in/deep/er/source.c $W/in/text.txt TO added QUIET
 check "$z add" "ziptest $W/a.zip && $ZI $W/a.zip | grep -q '^added/source.c|1|.*|8|'"
 rm -rf $W/zx && mkdir -p $W/zx && (cd $W/zx && unzip -q ../a.zip)
 check "$z add data" "cmp -s $W/in/deep/er/source.c $W/zx/added/source.c && cmp -s $W/in/text.txt $W/zx/added/text.txt"
 printf 'replaced\n' > $W/a.txt
 $AE $W/a.zip ADD $W/a.txt QUIET
 rm -rf $W/zx && mkdir -p $W/zx && (cd $W/zx && unzip -q ../a.zip)
 check "$z add replaces" "[ \$($ZI $W/a.zip | grep -c '^a.txt|') -eq 1 ] && cmp -s $W/a.txt $W/zx/a.txt"
done
check "zip archive comment kept" "cp tests/samples/z_amiga.zip $W/c.zip && $AE $W/c.zip a.txt DELETE QUIET && $ZI $W/c.zip | grep -q '^#amiga archive'"
# a new ZIP with drawers, empty drawer, random data (stored)
$AE $W/new.zip ADD $W/in QUIET
check "zip new archive" "ziptest $W/new.zip"
rm -rf $W/zx && mkdir -p $W/zx && (cd $W/zx && unzip -q ../new.zip)
for f in random.bin text.txt zeros.bin deep/binary emptyfile deep/er/one deep/er/source.c; do
 check "zip new data $f" "cmp -s $W/in/$f $W/zx/in/$f"
done
check "zip new empty drawer" "[ -d $W/zx/in/empty ]"
check "zip new random stored" "$ZI $W/new.zip | grep -q '^in/random.bin|1|.*|0|'"
check "zip info" "$AE $W/new.zip INFO | grep -q '^ZIP: DELETE RENAME COMMENT PROTECT ADD'"

# ---------------------------------------------------------------- TAR
TI="python3 $PWD/tests/tarinfo.py"
tartest() { tar -tf "$1" >/dev/null 2>&1 && bsdtar -tf "$1" >/dev/null 2>&1 && $TI "$1" >/dev/null 2>&1; }
LONG=sub/$(printf 'd%.0s' $(seq 1 60))/$(printf 'e%.0s' $(seq 1 50))   # the long drawer in the samples
for t in t_gnu t_pax t_ustar; do
 S=tests/samples/$t.tar
 cp $S $W/a.tar
 $AE $W/a.tar a.txt COMMENT "tar note" QUIET
 check "$t comment" "$TI $W/a.tar | grep -q '^a.txt|.*|tar note\$'"
 check "$t comment tar -t" "tartest $W/a.tar"
 $AE $W/a.tar sub/b.txt PROTECT rw QUIET
 check "$t protect" "$TI $W/a.tar | grep -q '^sub/b.txt|640|'"
 $AE $W/a.tar sub/b.txt PROTECT +e QUIET
 check "$t protect +e" "$TI $W/a.tar | grep -q '^sub/b.txt|740|'"
 cp $S $W/a.tar
 $AE $W/a.tar RENAME a.txt AS docs/first.txt QUIET
 $AE $W/a.tar RENAME sub AS other QUIET
 check "$t rename" "$TI $W/a.tar | grep -q '^docs/first.txt|' && $TI $W/a.tar | grep -q '^other/b.txt|' && ! $TI $W/a.tar | grep -q '^sub'"
 check "$t rename tar -t" "tartest $W/a.tar"
 rm -rf $W/tx && mkdir -p $W/tx && tar -xf $W/a.tar -C $W/tx
 check "$t rename data" "grep -q 'hello world' $W/tx/docs/first.txt && grep -q 'second file' $W/tx/other/b.txt"
 cp $S $W/a.tar
 $AE $W/a.tar sub DELETE QUIET
 check "$t delete drawer" "! $TI $W/a.tar | grep -q '^sub' && $TI $W/a.tar | grep -q '^a.txt|'"
 check "$t delete tar -t" "tartest $W/a.tar"
 $AE $W/a.tar ADD $W/in/deep/er/source.c TO added QUIET
 rm -rf $W/tx && mkdir -p $W/tx && tar -xf $W/a.tar -C $W/tx
 check "$t add data" "cmp -s $W/in/deep/er/source.c $W/tx/added/source.c"
done
# long names: rename into a name longer than 100 characters and back
cp tests/samples/t_gnu.tar $W/a.tar
$AE $W/a.tar RENAME a.txt AS "$LONG/renamed-$(printf 'x%.0s' $(seq 1 60)).txt" QUIET
check "tar long rename" "$TI $W/a.tar | grep -q 'renamed-xxxx' && tartest $W/a.tar"
rm -rf $W/tx && mkdir -p $W/tx && tar -xf $W/a.tar -C $W/tx
check "tar long rename data" "grep -q 'hello world' $W/tx/$LONG/renamed-*.txt && grep -q deep $W/tx/$LONG/long.txt"
# a new tar of the test tree
$AE $W/new.tar ADD $W/in QUIET
check "tar new archive" "tartest $W/new.tar"
rm -rf $W/tx && mkdir -p $W/tx && tar -xf $W/new.tar -C $W/tx
for f in random.bin text.txt zeros.bin deep/binary emptyfile deep/er/one deep/er/source.c; do
 check "tar new data $f" "cmp -s $W/in/$f $W/tx/in/$f"
done
check "tar new empty drawer" "[ -d $W/tx/in/empty ]"
check "tar info" "$AE $W/new.tar INFO | grep -q '^TAR: DELETE RENAME COMMENT PROTECT ADD'"

# ---------------------------------------------------------------- GZIP / TGZ
GI="python3 $PWD/tests/gzinfo.py"
cp tests/samples/g_note.gz $W/a.gz
$AE $W/a.gz a.txt COMMENT "new gz note" QUIET
check "gz comment" "$GI $W/a.gz | grep -q '^a.txt|new gz note\$' && gzip -t $W/a.gz"
$AE $W/a.gz RENAME a.txt AS renamed.txt QUIET
check "gz rename" "$GI $W/a.gz | grep -q '^renamed.txt|new gz note\$' && gzip -t $W/a.gz"
check "gz data kept" "gzip -dc $W/a.gz | grep -c 'hello world' | grep -q 10"
$AE $W/a.gz ADD $W/in/deep/er/source.c QUIET
check "gz add replaces" "gzip -dc $W/a.gz | cmp -s - $W/in/deep/er/source.c && $GI $W/a.gz | grep -q '^source.c|'"
$AE $W/a.gz a.txt DELETE QUIET >/dev/null
check "gz delete refused" "[ \$? -eq 10 ]"
$AE $W/a.gz ADD $W/in/text.txt $W/in/zeros.bin QUIET >/dev/null
check "gz two files refused" "[ \$? -eq 10 ]"
$AE $W/new.gz ADD $W/in/text.txt QUIET
check "gz new" "gzip -dc $W/new.gz | cmp -s - $W/in/text.txt"
check "gz info" "$AE $W/a.gz INFO | grep -q '^GZIP: RENAME COMMENT ADD\$'"

cp tests/samples/g_tar.tgz $W/a.tgz
$AE $W/a.tgz a.txt COMMENT "tgz note" QUIET
check "tgz comment" "gzip -t $W/a.tgz && gzip -dc $W/a.tgz > $W/a_.tar && $TI $W/a_.tar | grep -q '^a.txt|.*|tgz note\$'"
$AE $W/a.tgz RENAME sub AS other QUIET
$AE $W/a.tgz a.txt DELETE QUIET
$AE $W/a.tgz ADD $W/in/deep/er/source.c TO other QUIET
check "tgz tar -tz" "tar -tzf $W/a.tgz >/dev/null 2>&1"
rm -rf $W/tx && mkdir -p $W/tx && tar -xzf $W/a.tgz -C $W/tx
check "tgz edits" "[ ! -e $W/tx/a.txt ] && grep -q 'second file' $W/tx/other/b.txt && cmp -s $W/in/deep/er/source.c $W/tx/other/source.c"
check "tgz no temp files left" "! ls $W/a.tgz.arcedit* >/dev/null 2>&1"
$AE $W/new.tar.gz ADD $W/in QUIET
rm -rf $W/tx && mkdir -p $W/tx && tar -xzf $W/new.tar.gz -C $W/tx
check "tgz new" "cmp -s $W/in/deep/binary $W/tx/in/deep/binary && cmp -s $W/in/text.txt $W/tx/in/text.txt"
check "tgz info" "$AE $W/a.tgz INFO | grep -q '^TGZ: DELETE RENAME COMMENT PROTECT ADD'"

# ---------------------------------------------------------------- RAR (5)
rartest() { unrar t "$1" 2>&1 | grep -q '^All OK'; }
for r in r_plain r_big; do
 S=tests/samples/$r.rar
 cp $S $W/a.rar
 first=$(unrar lb $S | head -1)
 $AE $W/a.rar RENAME "$first" AS renamed/first QUIET
 check "$r rename" "rartest $W/a.rar && unrar lb $W/a.rar | grep -q '^renamed/first\$'"
 $AE $W/a.rar renamed/first PROTECT r QUIET
 check "$r protect" "rartest $W/a.rar && unrar l $W/a.rar | grep -q '^ -r--...... .*renamed/first'"
 cp $S $W/a.rar
 $AE $W/a.rar "$first" DELETE QUIET
 check "$r delete" "rartest $W/a.rar && ! unrar lb $W/a.rar | grep -qx '$first'"
 $AE $W/a.rar ADD $W/in/deep/er/source.c TO added QUIET
 rm -rf $W/rx && mkdir -p $W/rx && (cd $W/rx && unrar x -idq ../a.rar)
 check "$r add stored" "rartest $W/a.rar && cmp -s $W/in/deep/er/source.c $W/rx/added/source.c"
done
cp tests/samples/r_plain.rar $W/a.rar
$AE $W/a.rar RENAME sub AS other QUIET
check "rar rename drawer" "rartest $W/a.rar && unrar lb $W/a.rar | grep -q '^other/b.txt\$' && unrar lb $W/a.rar | grep -qx other"
rm -rf $W/rx && mkdir -p $W/rx && (cd $W/rx && unrar x -idq ../a.rar)
check "rar rename data" "grep -q 'second file' $W/rx/other/b.txt"
cp tests/samples/r_solid.rar $W/a.rar
$AE $W/a.rar RENAME a.txt AS b-first.txt QUIET
check "rar solid rename" "rartest $W/a.rar && unrar lb $W/a.rar | grep -q '^b-first.txt\$'"
$AE $W/a.rar sub DELETE QUIET >/dev/null
check "rar solid delete refused" "[ \$? -eq 10 ]"
check "rar solid info" "$AE $W/a.rar INFO | grep -q '^RAR: RENAME PROTECT\$'"
if [ -f tests/samples/r_v4.rar ]; then
 $AE tests/samples/r_v4.rar INFO >/dev/null
 check "rar 4 refused" "[ \$? -eq 10 ]"
fi
$AE $W/new.rar ADD $W/in/text.txt QUIET >/dev/null
check "rar new refused" "[ \$? -eq 10 ]"

# ---------------------------------------------------------------- LZX
XI="python3 $PWD/tests/lzxinfo.py"
lzxtest() { lsar -t "$1" 2>&1 | grep -q ' 0 failed' && $XI "$1" >/dev/null 2>&1; }
S=tests/samples/x_mix.lzx
cp $S $W/a.lzx
$AE $W/a.lzx a.txt COMMENT "lzx note" QUIET
$AE $W/a.lzx sub/b.txt PROTECT +s-r QUIET
$AE $W/a.lzx RENAME m AS merged QUIET
check "lzx header edits" "lzxtest $W/a.lzx && $XI $W/a.lzx | grep -q '^a.txt|.*|lzx note\$' && $XI $W/a.lzx | grep -q '^sub/b.txt|-s----ed|' && $XI $W/a.lzx | grep -q '^merged/two.txt|.*|1|'"
rm -rf $W/lx && mkdir -p $W/lx && (cd $W/lx && unar -q -D ../a.lzx)
check "lzx data after edits" "grep -q 'merged two' $W/lx/merged/two.txt && grep -q 'hello world' $W/lx/a.txt"
$AE $W/a.lzx merged/one.txt DELETE QUIET >/dev/null
check "lzx part of merged group refused" "[ \$? -eq 10 ] && $XI $W/a.lzx | grep -q '^merged/one.txt|'"
check "lzx no temp file left" "! ls $W/a.lzx.arcedit >/dev/null 2>&1"
$AE $W/a.lzx merged DELETE QUIET
check "lzx whole merged group" "lzxtest $W/a.lzx && ! $XI $W/a.lzx | grep -q '^merged/'"
$AE $W/a.lzx sub/b.txt DELETE QUIET
check "lzx delete" "lzxtest $W/a.lzx && [ \$($XI $W/a.lzx | wc -l) -eq 1 ]"
$AE $W/a.lzx ADD $W/in/deep/er/source.c TO added QUIET
rm -rf $W/lx && mkdir -p $W/lx && (cd $W/lx && unar -q -D ../a.lzx)
check "lzx add stored" "lzxtest $W/a.lzx && cmp -s $W/in/deep/er/source.c $W/lx/added/source.c"
$AE $W/new.lzx ADD $W/in/text.txt QUIET >/dev/null
check "lzx new refused" "[ \$? -eq 10 ]"
# archives made by LZX 1.21 (real_lzA: merged group + drawer entry,
# real_lzB: SYS:S packed -3, real_lzC: stored)
cp tests/samples/real_lzA.lzx $W/a.lzx
$AE $W/a.lzx sub/c.txt COMMENT "merged note" QUIET
$AE $W/a.lzx sub/b.txt PROTECT +p QUIET
$AE $W/a.lzx RENAME sub AS Drawer QUIET
$AE $W/a.lzx empty DELETE QUIET
check "real lzx edits" "lzxtest $W/a.lzx && $XI $W/a.lzx | grep -q '^Drawer/c.txt|.*|1|merged note\$' && $XI $W/a.lzx | grep -q '^Drawer/b.txt|--p-r-ed|' && ! $XI $W/a.lzx | grep -q '^empty'"
$AE $W/a.lzx Drawer/d.txt DELETE QUIET >/dev/null
check "real lzx partial group refused" "[ \$? -eq 10 ]"
$AE $W/a.lzx a.txt Drawer DELETE QUIET
check "real lzx whole group" "lzxtest $W/a.lzx && [ \$($XI $W/a.lzx | wc -l) -eq 0 ]"
cp tests/samples/real_lzB.lzx $W/b.lzx
$AE $W/b.lzx RENAME S AS Scripts QUIET
check "real lzx packed rename" "lzxtest $W/b.lzx && [ \$($XI $W/b.lzx | grep -c '^Scripts/') -eq 19 ]"
cp tests/samples/real_lzC.lzx $W/c.lzx
$AE $W/c.lzx sub/b.txt DELETE QUIET
$AE $W/c.lzx ADD $W/in/deep/er/source.c QUIET
rm -rf $W/lx && mkdir -p $W/lx && (cd $W/lx && unar -q -D ../c.lzx)
check "real lzx stored delete+add" "lzxtest $W/c.lzx && cmp -s $W/in/deep/er/source.c $W/lx/source.c && ! $XI $W/c.lzx | grep -q '^sub/b.txt'"

echo "passed $pass, failed $fail"
[ $fail -eq 0 ]
