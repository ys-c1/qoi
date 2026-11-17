#!/usr/bin/env bash
# run_premul_tests.sh — builds and runs the premultiplied-alpha test suite
# for qoi.h, plus the repo's existing checks (qoifuzz, qoiconv, qoibench).
#
# Usage (from the repo root):  bash run_premul_tests.sh
#
# No network access, no new dependencies: the test itself needs only qoi.h
# and libc. qoiconv/qoibench additionally need stb_image.h/stb_image_write.h
# which the upstream repo never vendored; if found elsewhere on this machine
# they are used via a -I flag (the repo itself is not modified for this).

set -euo pipefail
cd "$(dirname "$0")"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/samples" "$TMP/img"

CC=${CC:-gcc}

echo "=== 1. build reference encoder from unmodified qoi.h (git HEAD) ==="
git show HEAD:qoi.h > "$TMP/qoi_orig.h"
cat > "$TMP/qoi_orig.c" <<'EOF'
#define QOI_IMPLEMENTATION
#define QOI_NO_STDIO
#include "qoi_orig.h"
EOF
$CC -std=c99 -O2 -I"$TMP" \
	-Dqoi_encode=qoi_orig_encode -Dqoi_decode=qoi_orig_decode \
	-c "$TMP/qoi_orig.c" -o "$TMP/qoi_orig.o"
echo "ok"

echo
echo "=== 2. premultiplied-alpha test suite (default threshold) ==="
$CC -std=c99 -O2 -Wall -Wextra -I. qoi_premul_test.c "$TMP/qoi_orig.o" -o "$TMP/qoi_premul_test"
"$TMP/qoi_premul_test" "$TMP/samples"

echo
echo "=== 3. same suite with -DQOI_PREMULTIPLY_MIN_EDGE=10000 (threshold configurable) ==="
$CC -std=c99 -O2 -Wall -Wextra -I. -DQOI_PREMULTIPLY_MIN_EDGE=10000 \
	qoi_premul_test.c "$TMP/qoi_orig.o" -o "$TMP/qoi_premul_test_t"
"$TMP/qoi_premul_test_t"

echo
echo "=== 4. invalid thresholds must fail to compile ==="
cat > "$TMP/thresh_check.c" <<'EOF'
#define QOI_IMPLEMENTATION
#include "qoi.h"
int main(void) { return 0; }
EOF
for bad in 0 -1 1.5 abc; do
	if $CC -std=c99 -I. -DQOI_PREMULTIPLY_MIN_EDGE=$bad -c "$TMP/thresh_check.c" -o "$TMP/tc.o" 2>"$TMP/tc.log"; then
		echo "FAIL: threshold $bad compiled successfully"
		exit 1
	fi
	echo "  threshold $bad -> compile error (as required):"
	grep -m1 -i "error" "$TMP/tc.log" | sed 's/^/    /'
done

echo
echo "=== 5. existing repo check: qoifuzz ==="
if clang -fsanitize=address,fuzzer -g -O1 -I. qoifuzz.c -o "$TMP/qoifuzz" 2>"$TMP/fuzz_build.log"; then
	"$TMP/qoifuzz" "$TMP/samples" -max_total_time=30 -print_final_stats=1 2>&1 | tail -6
else
	echo "  clang libFuzzer runtime not installed; running qoifuzz.c under"
	echo "  gcc -fsanitize=address,undefined with a mutation driver instead"
	cat > "$TMP/fuzz_driver.c" <<'EOF'
/* feeds corpus files and random mutations to LLVMFuzzerTestOneInput */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size);

static unsigned long long s = 0x243F6A8885A308D3ull;
static unsigned int frnd(void) { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return (unsigned)s; }

#define MAXSZ 65536
static unsigned char buf[MAXSZ];

int main(int argc, char **argv) {
	DIR *d;
	struct dirent *de;
	long iter, iters = (argc > 2) ? atol(argv[2]) : 200000;
	size_t len = 0;
	int files = 0;

	if (argc < 2 || !(d = opendir(argv[1]))) { fprintf(stderr, "usage: %s <corpusdir> [iters]\n", argv[0]); return 1; }
	while ((de = readdir(d))) {
		if (de->d_name[0] == '.') { continue; }
		char path[1024];
		snprintf(path, sizeof(path), "%s/%s", argv[1], de->d_name);
		FILE *f = fopen(path, "rb");
		if (!f) { continue; }
		size_t n = fread(buf, 1, MAXSZ, f);
		fclose(f);
		LLVMFuzzerTestOneInput(buf, n);
		if (n > len) { len = n; }
		files++;
	}
	closedir(d);
	for (iter = 0; iter < iters; iter++) {
		size_t n = frnd() % (len ? len : 64) + 4, j, m = 1 + frnd() % 8;
		for (j = 0; j < m; j++) { buf[frnd() % n] = (unsigned char)frnd(); }
		LLVMFuzzerTestOneInput(buf, n);
	}
	printf("qoifuzz: %d seed files + %ld mutated inputs, no crashes\n", files, iters);
	return 0;
}
EOF
	gcc -std=c99 -O1 -g -fsanitize=address,undefined -I. qoifuzz.c "$TMP/fuzz_driver.c" -o "$TMP/qoifuzz"
	"$TMP/qoifuzz" "$TMP/samples" 200000
fi

echo
echo "=== 6. existing repo build: make (qoibench/qoiconv need stb_image.h) ==="
make clean >/dev/null 2>&1 || true
STB_DIR=""
for d in /mnt/wslg/distro/home/ys/tasks/projects/qoi /tmp/snaps/stb-1 /usr/local/include /usr/include; do
	if [ -f "$d/stb_image.h" ] && [ -f "$d/stb_image_write.h" ]; then STB_DIR="$d"; break; fi
done
if ! make 2>"$TMP/make.log"; then
	echo "  plain 'make' fails (pre-existing, unmodified HEAD behaves identically):"
	grep -m1 "stb_image" "$TMP/make.log" | sed 's/^/    /'
fi
if [ -n "$STB_DIR" ]; then
	echo "  stb headers found at $STB_DIR — building tools with -I (repo unchanged)"
	make CFLAGS="-I$STB_DIR"
else
	echo "  stb headers not found on this machine; skipping qoiconv/qoibench"
	exit 0
fi

echo
echo "=== 7. existing repo check: qoiconv round trip qoi -> png -> qoi ==="
for f in "$TMP/samples"/*.qoi; do
	name=$(basename "$f" .qoi)
	./qoiconv "$f" "$TMP/img/$name.png" >/dev/null
	./qoiconv "$TMP/img/$name.png" "$TMP/img/$name.roundtrip.qoi" >/dev/null
	cmp "$f" "$TMP/img/$name.roundtrip.qoi" && echo "  $name: qoi->png->qoi byte-identical"
done

echo
echo "=== 8. existing repo check: qoibench (verifies qoi roundtrip per image) ==="
./qoibench 3 "$TMP/img" --onlytotals

make clean >/dev/null 2>&1 || true

echo
echo "ALL TEST RUNS COMPLETED SUCCESSFULLY"
