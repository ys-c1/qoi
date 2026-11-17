#!/bin/sh
#
# One-shot test runner for the near-lossless QOI extension.
# Does not touch the Makefile; uses only the system C compiler and libc.
#
#   ./run_tests.sh
#
# Exits non-zero if any test step fails.

set -e

cd "$(dirname "$0")"

CC="${CC:-cc}"
CFLAGS="-std=c89 -pedantic -Wall -Wextra -Werror -O2"
OUT="$(mktemp -d)/qoi_nl_test"

echo "== Compiling qoi_nl_test.c ($CC $CFLAGS) =="
$CC $CFLAGS qoi_nl_test.c -o "$OUT"

echo "== Running functional + benchmark harness =="
"$OUT"
status=$?

echo
echo "== Re-compiling qoi.h under extra dialects / sanitizers =="
tmpc="$(mktemp -d)/sanity.c"
printf '#define QOI_IMPLEMENTATION\n#include "qoi.h"\nint main(void){return 0;}\n' \
	> "$tmpc"
gcc -std=c89 -pedantic -Wall -Wextra      -I. -c "$tmpc" -o "$tmpc.c89.o"
gcc -std=c99 -Wall -Wextra -Werror        -I. -c "$tmpc" -o "$tmpc.c99.o"
g++ -x c++ -I. -Wall -Wextra              -c "$tmpc" -o "$tmpc.cpp.o"
gcc -std=c99 -I. -fsanitize=address,undefined -g -O1 \
	qoi_nl_test.c -o "$OUT.asan"
echo "== Running harness under ASan + UBSan =="
"$OUT.asan" >/dev/null
echo "sanitizer run clean"

rm -f "$OUT" "$OUT.asan"
exit $status
