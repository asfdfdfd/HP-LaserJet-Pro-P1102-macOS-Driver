#!/bin/bash
# run_tests.sh - byte-identity tests for the rastertozjs filter.
# Builds a reference converter from the vendored zjs engine (tests/refmain.c),
# feeds both the filter and the reference the same bitmap, and requires the
# produced ZjStream documents to be byte-identical (PJL timestamp normalized).
set -e
cd "$(dirname "$0")/.."

SDK=$(xcrun --show-sdk-path)
CC=clang
CFLAGS="-O2 -Wall -I src/zjs -isysroot $SDK"
LIBS="-isysroot $SDK -lcupsimage -lcups -lpthread"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

echo "==> building..."
make >/dev/null
$CC $CFLAGS -o $TMP/refmain tests/refmain.c src/zjs/vendor/foo2zjs.c \
    src/zjs/vendor/jbig.c src/zjs/vendor/jbig_ar.c $LIBS
$CC $CFLAGS -o $TMP/mkraster tests/mkraster.c $LIBS

echo "==> generating fixtures..."
python3 tests/mkfixtures.py "$TMP"

pass=0
fail=0

check() {
    local name=$1 pbm=$2 raster=$3 args=$4 refargs=$5
    if [ -z "$6" ]; then
        $TMP/mkraster $args < "$TMP/$pbm" > "$TMP/$raster"
    fi
    build/rastertozjs 1 test "$name" 1 "" < "$TMP/$raster" > "$TMP/out_$name.zjs"
    $TMP/refmain $refargs < "$TMP/$pbm" > "$TMP/ref_$name.zjs"
    if python3 -c '
import re, sys
a = re.sub(rb"JobAttr4=\d{14}", b"JobAttr4=N", open(sys.argv[1], "rb").read())
b = re.sub(rb"JobAttr4=\d{14}", b"JobAttr4=N", open(sys.argv[2], "rb").read())
sys.exit(0 if a == b else 1)
' "$TMP/out_$name.zjs" "$TMP/ref_$name.zjs"; then
        echo "PASS  $name"
        pass=$((pass + 1))
    else
        echo "FAIL  $name"
        fail=$((fail + 1))
    fi
}

check "letter600"   test600.pbm   test600.raster   ""   "-z2 -P -L0 -r600x600 -g5100x6600 -p1 -m1 -s7 -n1"
check "noise1200"   testnoise.pbm testnoise.raster "-r 1200x600" "-z2 -P -L0 -r1200x600 -g10200x6600 -p1 -m1 -s7 -n1"
check "twopages"    two.pbm       two.raster       ""   "-z2 -P -L0 -r600x600 -g5100x6600 -p1 -m1 -s7 -n1"
# Grayscale: threshold the PGM the same way the filter does (px < 128 -> black)
python3 -c '
import sys
src = open(sys.argv[1] + "/gray.pgm", "rb").read()
import re
m = re.match(rb"P5\s+(\d+)\s+(\d+)\s+\d+\s*\n", src)
w, h = int(m.group(1)), int(m.group(2))
data = src[m.end():]
out = bytearray(b"P4\n%d %d\n" % (w, h))
bpl = (w + 7) // 8
for y in range(h):
    row = data[y*w:(y+1)*w]
    r = bytearray(bpl)
    for x in range(w):
        if row[x] < 128:
            r[x // 8] |= 0x80 >> (x & 7)
    out += r
open(sys.argv[1] + "/gray_thr.pbm", "wb").write(out)
' "$TMP"
$TMP/mkraster -g < "$TMP/gray.pgm" > "$TMP/gray.raster"
check "gray"        gray_thr.pbm  gray.raster      "-g" "-z2 -P -L0 -r600x600 -g5100x6600 -p1 -m1 -s7 -n1" prebuilt

echo
echo "PASS: $pass  FAIL: $fail"
[ $fail -eq 0 ]
