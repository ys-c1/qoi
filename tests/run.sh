#!/bin/sh
# Tests for the qoi content-fingerprint encode cache.
#
# Runs with only the toolchain already required by this repository (a C
# compiler, make and libpng for qoibench). No third-party downloads, no network
# access, and nothing outside this repo's own sources.
#
#   1. compile the cache test/benchmark and run it (exit code 0 = pass);
#   2. prove qoi_encode() output is byte-for-byte unchanged: build the same
#      dump program against the pristine qoi.h from git HEAD and against the
#      working tree, then cmp the two corpora;
#   3. compile/run the repo's existing qoifuzz.c harness against the working
#      tree (the repo has no other unit tests);
#   4. build the existing targets from the unmodified Makefile (qoiconv,
#      qoibench) and convert a tiny generated PNG round-trip through qoi, so
#      "existing repo tests/build" are exercised too. qoiconv/qoibench need
#      stb_image.h / stb_image_write.h, which this repo's .gitignore excludes
#      and which are not vendored in this checkout; if they are absent the step
#      is SKIP (a missing optional dependency, not a failure). A present-but-
#      broken build is FAIL. If both are present, qoibench is also run.
#
# Usage: tests/run.sh   (from anywhere; it resolves the repo root itself)

set -u

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "$here/.." && pwd)
out="$root/tests/.out"
cc=${CC:-cc}

mkdir -p "$out" || exit 1
fail=0
skip=0

say()  { printf '%s\n' "$*"; }
fail_msg() { printf 'FAIL: %s\n' "$*"; fail=1; }
skip_msg() { printf 'SKIP: %s\n' "$*"; skip=1; }

cleanup() {
	rm -rf "$out"
}
trap cleanup EXIT INT TERM

say "== 1. cache test/benchmark =="
if $cc -std=c99 -O2 -Wall -Wextra -pedantic -Werror -I"$root" \
		"$here/qoi_cache_test.c" -o "$out/qoi_cache_test"; then
	if "$out/qoi_cache_test"; then
		say "PASS: cache tests"
	else
		fail_msg "cache tests"
	fi
else
	fail_msg "cache test build"
fi

say ""
say "== 2. qoi_encode() byte-for-byte unchanged vs git HEAD =="
if git -C "$root" rev-parse --verify HEAD >/dev/null 2>&1; then
	git -C "$root" show HEAD:qoi.h > "$out/qoi_head.h" || fail_msg "git show HEAD:qoi.h"
	# The dump includes "qoi.h" from its own directory; copy the sources next
	# to the header each build should see.
	mkdir -p "$out/head" "$out/work"
	cp "$out/qoi_head.h" "$out/head/qoi.h"
	cp "$root/qoi.h" "$out/work/qoi.h"
	cp "$here/encode_dump.c" "$out/head/" 2>/dev/null || true
	cp "$here/encode_dump.c" "$out/work/" 2>/dev/null || true
	if $cc -std=c99 -O2 -Wall -Wextra -pedantic -Werror "$out/head/encode_dump.c" -o "$out/dump_head" &&
	   $cc -std=c99 -O2 -Wall -Wextra -pedantic -Werror "$out/work/encode_dump.c" -o "$out/dump_work"; then
		if "$out/dump_head" "$out/head.bin" && "$out/dump_work" "$out/work.bin"; then
			if cmp -s "$out/head.bin" "$out/work.bin"; then
				say "PASS: encode corpus identical ($(wc -c < "$out/work.bin") bytes)"
			else
				fail_msg "qoi_encode output differs between HEAD and working tree"
			fi
		else
			fail_msg "encode dump run"
		fi
	else
		fail_msg "encode dump build"
	fi
else
	skip_msg "git HEAD unavailable; byte-for-byte comparison skipped"
fi

say ""
say "== 3. existing fuzz harness (qoifuzz.c) still builds/runs =="
if [ -f "$root/qoifuzz.c" ]; then
	# qoifuzz.c is an LLVMFuzzerTestOneInput harness. Without libFuzzer, build
	# it with a tiny main that feeds it a few structured inputs.
	cat > "$out/fuzz_driver.c" <<'EOF'
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
int main(void) {
	/* minimal valid QOI header + padding and some malformed variants */
	static uint8_t cases[][32] = {
		{'q','o','i','f',0,0,0,1,0,0,0,1,4,0, 0xfe,1,2,3, 0,0,0,0,0,0,0,1},
		{'q','o','i','f',0,0,0,0,0,0,0,0,4,0},
		{'q','o','i','f',0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,4,0},
		{0},
	};
	int channels[] = {0, 3, 4, 3};
	int i;
	for (i = 0; i < 4; i++) {
		uint8_t buf[36];
		buf[0] = (uint8_t)channels[i]; buf[1] = 0; buf[2] = 0; buf[3] = 0;
		for (size_t j = 0; j < 32; j++) buf[4 + j] = cases[i][j];
		LLVMFuzzerTestOneInput(buf, sizeof(buf));
	}
	puts("fuzz driver ok");
	return 0;
}
EOF
	# Rename main conflict-free: qoifuzz.c has no main, so link directly.
	if $cc -std=c99 -O1 -g -I"$root" "$root/qoifuzz.c" "$out/fuzz_driver.c" \
			-o "$out/qoifuzz" 2>"$out/fuzz_build.log"; then
		if "$out/qoifuzz"; then
			say "PASS: qoifuzz harness decodes crafted inputs without crashing"
		else
			fail_msg "qoifuzz harness crashed"
		fi
	else
		fail_msg "qoifuzz harness build (see log)"
		sed 's/^/    /' "$out/fuzz_build.log" | head -20
	fi
else
	skip_msg "qoifuzz.c not present"
fi

say ""
say "== 4. existing Makefile targets =="
# The Makefile expects stb headers next to the sources. This checkout's
# .gitignore excludes them and they are not vendored; a copy elsewhere on the
# machine (e.g. a sibling stb checkout) is acceptable as an optional input, but
# nothing is downloaded. If they are unavailable the step is SKIP.
have_stb=0
if [ -f "$root/stb_image.h" ] && [ -f "$root/stb_image_write.h" ]; then
	have_stb=1
else
	for dir in "${STB_DIR:-}" /home/ys/tasks/projects/qoi \
			"$root/../stb" "$root/../qoi"; do
		[ -n "$dir" ] || continue
		if [ -f "$dir/stb_image.h" ] && [ -f "$dir/stb_image_write.h" ]; then
			mkdir -p "$out/build"
			cp "$root/Makefile" "$root/qoiconv.c" "$root/qoibench.c" \
				"$root/qoi.h" "$dir/stb_image.h" "$dir/stb_image_write.h" \
				"$out/build/" || fail_msg "stage stb build"
			build_dir="$out/build"
			have_stb=1
			break
		fi
	done
fi
if [ "$have_stb" -eq 1 ]; then
	if [ -z "${build_dir:-}" ]; then
		build_dir="$root"
		make -C "$root" clean >/dev/null 2>&1
	fi
	if make -C "$build_dir" all >"$out/make.log" 2>&1; then
		say "PASS: make all (qoiconv, qoibench)"
		# Round-trip a generated 4x4 PNG through qoiconv: png -> qoi -> png ->
		# qoi, and verify the two QOI files are identical.
		cat > "$out/mkpng.c" <<'EOF'
#include <png.h>
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv) {
	FILE *f;
	png_structp png;
	png_infop info;
	unsigned char px[4 * 4 * 4];
	int x, y;
	for (y = 0; y < 4; y++) for (x = 0; x < 4; x++) {
		unsigned char *p = px + (y * 4 + x) * 4;
		p[0] = (unsigned char)(x * 40);
		p[1] = (unsigned char)(y * 50);
		p[2] = (unsigned char)((x + y) * 20);
		p[3] = 255;
	}
	f = fopen(argv[1], "wb");
	if (!f) return 1;
	png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
	info = png_create_info_struct(png);
	if (setjmp(png_jmpbuf(png))) return 2;
	png_init_io(png, f);
	png_set_IHDR(png, info, 4, 4, 8, PNG_COLOR_TYPE_RGBA,
		PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
	png_write_info(png, info);
	for (y = 0; y < 4; y++) png_write_row(png, px + y * 4 * 4);
	png_write_end(png, NULL);
	png_destroy_write_struct(&png, &info);
	fclose(f);
	return 0;
}
EOF
	if $cc -std=c99 -O2 "$out/mkpng.c" -o "$out/mkpng" -lpng 2>"$out/png.log"; then
		if "$out/mkpng" "$out/in.png" &&
		   "$build_dir/qoiconv" "$out/in.png" "$out/a.qoi" &&
		   "$build_dir/qoiconv" "$out/a.qoi" "$out/b.png" &&
		   "$build_dir/qoiconv" "$out/b.png" "$out/b.qoi"; then
			if cmp -s "$out/a.qoi" "$out/b.qoi"; then
				say "PASS: qoiconv png->qoi->png->qoi round-trip is stable"
			else
				fail_msg "qoiconv round-trip produced different QOI bytes"
			fi
		else
			fail_msg "qoiconv round-trip"
		fi
		# qoibench has its own verify pass (qoi roundtrip); run it on the
		# generated image so the repo's other tool is exercised too.
		mkdir -p "$out/images"
		cp "$out/in.png" "$out/images/in.png"
		if "$build_dir/qoibench" 1 "$out/images" --nopng >"$out/bench.log" 2>&1; then
			say "PASS: qoibench runs and verifies its QOI roundtrip"
		else
			fail_msg "qoibench"
			sed 's/^/    /' "$out/bench.log" | tail -20
		fi
	else
		skip_msg "libpng not available for generated PNG round-trip"
	fi
else
	fail_msg "make all"
	sed 's/^/    /' "$out/make.log" | head -20
fi
else
	skip_msg "make all needs optional vendored stb headers (absent in checkout; not part of this change)"
fi

say ""
if [ "$fail" -ne 0 ]; then
	say "RESULT: FAILURES"
	exit 1
fi
if [ "$skip" -ne 0 ]; then
	say "RESULT: all executed checks passed (some optional steps skipped)"
else
	say "RESULT: all checks passed"
fi
exit 0
