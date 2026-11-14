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

echo "passed $pass, failed $fail"
[ $fail -eq 0 ]
