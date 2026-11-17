#!/bin/sh
# One-shot test runner for the layered (progressive pyramid) QOI codec.
#
# Builds and runs the self-contained functional + metrics + timing test in
# qoi_py_test.c. All sample images are synthesized inside that program, so
# this needs no image files, no third-party libraries and no network.
#
# Also compiles the existing qoibench.c / qoiconv.c when the optional stb
# headers are available somewhere on the include path (they are intentionally
# not shipped in this repo, see .gitignore). A missing stb is reported, not
# treated as a failure of the layered-codec work.
#
# Exit status is non-zero if any required check fails.

set -e
cd "$(dirname "$0")"

CC="${CC:-cc}"
CFLAGS="${CFLAGS:--std=c99 -O3 -Wall -Wextra}"

echo "== 1/3  layered codec functional/metrics/timing test =="
$CC $CFLAGS qoi_py_test.c -o qoi_py_test
./qoi_py_test

echo
echo "== 2/3  AddressSanitizer + UBSan run =="
$CC -std=c99 -O1 -g -fsanitize=address,undefined qoi_py_test.c -o qoi_py_test_asan
./qoi_py_test_asan >/dev/null
echo "ASan/UBSan: clean"

echo
echo "== 3/3  existing tools still build against the modified qoi.h =="
# Find stb headers outside this repo without copying anything in.
STB_INC=""
for d in /tmp/qoi_stb /tmp/stbstub; do
	if [ -f "$d/stb_image.h" ] && [ -f "$d/stb_image_write.h" ]; then
		STB_INC="$d"
		break
	fi
done
if [ -n "$STB_INC" ]; then
	$CC -std=gnu99 -O3 -I"$STB_INC" qoibench.c -o /tmp/qoibench_check -lpng
	$CC -std=c99  -O3 -I"$STB_INC" qoiconv.c  -o /tmp/qoiconv_check
	echo "qoibench + qoiconv build: ok (stb from $STB_INC)"
else
	echo "qoibench/qoiconv: skipped (stb_image headers not present; repo never ships them)"
fi

echo
echo "ALL SCRIPT CHECKS PASSED"
