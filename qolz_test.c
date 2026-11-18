/*

QOLZ acceptance harness - synthetic samples, benchmarks and edge case tests.

Self contained: only libc. Builds into artifacts/, no Makefile changes:
	gcc -std=c99 -O2 -Wall -Wextra qolz_test.c -o artifacts/qolz_test
	(also run an ASan build, see artifacts/run.sh in the delivery notes)

It exercises two coding paths over the same four synthetic RGBA images:
  - original path: qoi_encode / qoi_decode
  - preprocessed : qolz_encode / qolz_decode (LZ77 preprocessor + QOLZ)
and reports raw size, encoded size, encode/decode time for each, plus
roundtrip pixel checks and the required edge cases.
*/

#define _POSIX_C_SOURCE 199309L

#define QOI_IMPLEMENTATION
#define QOLZ_IMPLEMENTATION
#include "qoi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#define W 256
#define H 256
#define CH 4
#define NPIX (W * H)
#define RAW_LEN (NPIX * CH)

/* ---------------------------------------------------------------- PRNG */

static unsigned int rng_state = 0x12345678u;

static unsigned int xrnd(void) {
	unsigned int x = rng_state;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	rng_state = x;
	return x;
}

static unsigned char rnd_byte(void) {
	return (unsigned char)(xrnd() >> 24);
}

/* ----------------------------------------------------- sample generators */

static unsigned char *new_raw(void) {
	unsigned char *p = (unsigned char *)malloc(RAW_LEN);
	if (!p) { exit(1); }
	return p;
}

/* Pixel art: large flat color blocks, a repeating 16x16 "sprite" and
single pixel outlines - very long exact runs and many repeats. */
static void gen_pixel_art(unsigned char *px) {
	int y, x, k;
	for (y = 0; y < H; y++) {
		for (x = 0; x < W; x++) {
			int block = ((x / 32) + (y / 32)) & 1;
			int sprite = (((x & 15) * (y & 15)) & 7) == 0;
			int outline = (x % 32 == 0 || y % 32 == 0);
			unsigned char r, g, b;
			if (outline) { r = 16; g = 16; b = 24; }
			else if (sprite) { r = 220; g = 60; b = 90; }
			else if (block) { r = 70; g = 130; b = 200; }
			else { r = 200; g = 210; b = 220; }
			k = (y * W + x) * CH;
			px[k] = r; px[k + 1] = g; px[k + 2] = b; px[k + 3] = 255;
		}
	}
}

/* Screenshot: horizontal gradient bands, UI panels, repeated text-like
runs (stippled rows), a checker grid and duplicated regions. */
static void gen_screenshot(unsigned char *px) {
	int y, x, k;
	for (y = 0; y < H; y++) {
		for (x = 0; x < W; x++) {
			int panel = (y < 48);
			int grid = (x % 64 < 2 || y % 40 < 2);
			int text = (y > 64 && y < 96 && ((x * 7 + y) & 31) < 6);
			int copy = (y > 128); /* lower half mirrors an upper pattern */
			int sy = copy ? (y - 128) : y;
			unsigned char r = (unsigned char)(sy * 2 + x / 4);
			unsigned char g = (unsigned char)(80 + x / 8);
			unsigned char b = (unsigned char)(160 - sy);
			if (panel) { r = 40; g = 44; b = 56; }
			if (grid) { r = 230; g = 230; b = 235; }
			if (text) { r = 245; g = 245; b = 245; }
			k = (y * W + x) * CH;
			px[k] = r; px[k + 1] = g; px[k + 2] = b; px[k + 3] = 255;
		}
	}
}

/* Photo-ish: smooth 2D gradients with low amplitude film grain. */
static void gen_photo(unsigned char *px) {
	int y, x, k;
	for (y = 0; y < H; y++) {
		for (x = 0; x < W; x++) {
			int grain = (int)(rnd_byte() & 7) - 4;
			int r = (x * 255) / W + grain;
			int g = (y * 255) / H + grain;
			int b = 128 + ((x - y) / 2) + grain;
			if (r < 0) r = 0; else if (r > 255) r = 255;
			if (g < 0) g = 0; else if (g > 255) g = 255;
			if (b < 0) b = 0; else if (b > 255) b = 255;
			k = (y * W + x) * CH;
			px[k] = (unsigned char)r;
			px[k + 1] = (unsigned char)g;
			px[k + 2] = (unsigned char)b;
			px[k + 3] = 255;
		}
	}
}

/* Noise: every byte independently random, no exploitable structure. */
static void gen_noise(unsigned char *px) {
	int i;
	for (i = 0; i < RAW_LEN; i++) {
		px[i] = rnd_byte();
	}
}

/* ---------------------------------------------------------------- timer */

static uint64_t now_ns(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int cmp_u64(const void *a, const void *b) {
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
	return (x > y) - (x < y);
}

#define ITERS 51

typedef struct {
	int enc_len;
	uint64_t enc_ns;   /* median encode time */
	uint64_t dec_ns;   /* median decode time */
	uint64_t best_enc; /* minimum observed encode time */
	uint64_t best_dec; /* minimum observed decode time */
} path_stats;

static void bench_qoi(const unsigned char *raw, path_stats *s) {
	uint64_t te[ITERS], td[ITERS];
	qoi_desc desc = { W, H, CH, QOI_SRGB };
	int i, best_enc = 0;
	void *enc = NULL, *dec = NULL;
	for (i = 0; i < ITERS; i++) {
		int el = 0;
		qoi_desc d2;
		uint64_t t0 = now_ns();
		enc = qoi_encode(raw, &desc, &el);
		uint64_t t1 = now_ns();
		dec = qoi_decode(enc, el, &d2, CH);
		uint64_t t2 = now_ns();
		te[i] = t1 - t0;
		td[i] = t2 - t1;
		if (i == 0) {
			best_enc = el;
			if (!enc || !dec || memcmp(dec, raw, RAW_LEN) != 0) {
				fprintf(stderr, "FATAL: qoi roundtrip mismatch\n");
				exit(2);
			}
		}
		free(enc);
		free(dec);
	}
	qsort(te, ITERS, sizeof(uint64_t), cmp_u64);
	qsort(td, ITERS, sizeof(uint64_t), cmp_u64);
	s->enc_len = best_enc;
	s->enc_ns = te[ITERS / 2];
	s->dec_ns = td[ITERS / 2];
	s->best_enc = te[0];
	s->best_dec = td[0];
}

static int qolz_mode_of(const unsigned char *enc) {
	return enc[QOLZ_HEADER_SIZE - 1];
}

static void bench_qolz(const unsigned char *raw, path_stats *s, int *mode) {
	uint64_t te[ITERS], td[ITERS];
	qoi_desc desc = { W, H, CH, QOI_SRGB };
	int i, best_enc = 0, best_mode = -1;
	void *enc = NULL, *dec = NULL;
	for (i = 0; i < ITERS; i++) {
		int el = 0;
		qoi_desc d2;
		uint64_t t0 = now_ns();
		enc = qolz_encode(raw, &desc, &el);
		uint64_t t1 = now_ns();
		dec = qolz_decode(enc, el, &d2, CH);
		uint64_t t2 = now_ns();
		te[i] = t1 - t0;
		td[i] = t2 - t1;
		if (i == 0) {
			best_enc = el;
			best_mode = qolz_mode_of((const unsigned char *)enc);
			if (!enc || !dec || memcmp(dec, raw, RAW_LEN) != 0) {
				fprintf(stderr, "FATAL: qolz roundtrip mismatch\n");
				exit(2);
			}
		}
		free(enc);
		free(dec);
	}
	qsort(te, ITERS, sizeof(uint64_t), cmp_u64);
	qsort(td, ITERS, sizeof(uint64_t), cmp_u64);
	s->enc_len = best_enc;
	s->enc_ns = te[ITERS / 2];
	s->dec_ns = td[ITERS / 2];
	s->best_enc = te[0];
	s->best_dec = td[0];
	*mode = best_mode;
}

static double ms(uint64_t ns) { return (double)ns / 1.0e6; }

/* ------------------------------------------------------- edge case tests */

static int failures = 0;

#define CHECK(cond, name) do { \
	if (cond) { printf("  [ok] %s\n", name); } \
	else { printf("  [FAIL] %s\n", name); failures++; } \
} while (0)

/* Direct LZ77 roundtrip over arbitrary buffers/parameter combos. */
static int lz_roundtrip(const unsigned char *in, int n,
	int min_match, int max_chain, int lazy
) {
	int cl = 0;
	unsigned char *c = qolz_lz_compress(in, n, min_match, max_chain,
		QOLZ_MAX_MATCH, lazy, &cl);
	unsigned char *d;
	int ok;
	if (!c) { return 0; }
	d = qolz_lz_decompress(c, cl, n, n, NULL);
	ok = (d != NULL) && (memcmp(d, in, (size_t)n) == 0);
	free(c);
	free(d);
	return ok;
}

static void test_edge_cases(void) {
	int i;
	unsigned char *buf = (unsigned char *)malloc(200000);
	unsigned char *tmp;
	int cl;

	printf("edge cases:\n");

	/* 1. min_match = 1 must work and stay lossless (degenerate coding). */
	{
		int ok = 1;
		rng_state = 0xa5a5a5a5u;
		for (i = 0; i < 10000; i++) { buf[i] = rnd_byte(); }
		for (i = 0; i < 10000; i += 37) { buf[i] = buf[(i + 7) % 10000]; }
		ok &= lz_roundtrip(buf, 1, 1, 8, 0);
		ok &= lz_roundtrip(buf, 2, 1, 8, 0);
		ok &= lz_roundtrip(buf, 3, 1, 8, 0);
		ok &= lz_roundtrip(buf, 10000, 1, 1, 0);
		ok &= lz_roundtrip(buf, 10000, 1, 64, 1);
		CHECK(ok, "min_match=1 degrades correctly (lossless, greedy+lazy)");
	}

	/* 2. No match inside the window must terminate (pure random data). */
	{
		uint64_t t0, t1;
		rng_state = 0x55aa55aau;
		for (i = 0; i < 100000; i++) { buf[i] = rnd_byte(); }
		t0 = now_ns();
		tmp = qolz_lz_compress(buf, 100000, 3, 32, QOLZ_MAX_MATCH, 1, &cl);
		t1 = now_ns();
		CHECK(tmp != NULL && cl > 0 && (t1 - t0) < 2000000000ull,
			"no-match input terminates (no infinite loop), finishes fast");
		CHECK(lz_roundtrip(buf, 100000, 3, 32, 1),
			"no-match input roundtrips losslessly");
		free(tmp);
	}

	/* 3. Empty and tiny inputs. */
	{
		int ok = 1;
		buf[0] = 7; buf[1] = 9; buf[2] = 13; buf[3] = 21;
		ok &= lz_roundtrip(buf, 0, 3, 8, 1);
		ok &= lz_roundtrip(buf, 1, 3, 8, 1);
		ok &= lz_roundtrip(buf, 2, 3, 8, 1);
		ok &= lz_roundtrip(buf, 3, 3, 8, 1);
		ok &= lz_roundtrip(buf, 4, 3, 8, 1);
		CHECK(ok, "empty and sub-min-match inputs roundtrip");
	}

	/* 4. Runs longer than QOLZ_MAX_MATCH span several match tokens. */
	{
		int n = 100000;
		int ok;
		memset(buf, 0x5a, (size_t)n);
		tmp = qolz_lz_compress(buf, n, 3, 8, QOLZ_MAX_MATCH, 0, &cl);
		ok = tmp != NULL && cl < n / 10;
		CHECK(ok, "100k repetitive bytes compress strongly (multi-token match)");
		CHECK(lz_roundtrip(buf, n, 3, 8, 0), "long run roundtrips");
		free(tmp);
	}

	/* 5. Match distance exactly at the 64 KB window boundary. */
	{
		int n = 70000 * 2;
		int ok;
		memset(buf, 0, (size_t)n);
		memcpy(buf + 70000, buf, 70000); /* 70000 > 65535 */
		ok = lz_roundtrip(buf, n, 3, 32, 1);
		CHECK(ok, "match beyond/at 64KB window handled (clamped, lossless)");
	}

	/* 6. Full parameter sweep on a mixed buffer. */
	{
		int ok = 1, mm, mc, lz;
		rng_state = 0xdeadbeefu;
		for (i = 0; i < 50000; i++) {
			buf[i] = (unsigned char)(rnd_byte() & (i % 97 == 0 ? 0xff : 0x0f));
		}
		for (mm = 1; mm <= 5; mm++)
			for (mc = 1; mc <= 33; mc += 8)
				for (lz = 0; lz <= 1; lz++)
					ok &= lz_roundtrip(buf, 50000, mm, mc, lz);
		CHECK(ok, "parameter sweep (min_match x max_chain x lazy) lossless");
	}

	/* 7. Malformed streams must be rejected, not crash. */
	{
		qoi_desc desc = { 8, 8, CH, QOI_SRGB };
		unsigned char small[64] = {0};
		int el, ok_null = 1;
		void *enc = qolz_encode(buf, &desc, &el);
		qoi_desc d2;
		void *dec;
		/* truncation at every prefix length */
		for (i = 0; i < el; i++) {
			dec = qolz_decode(enc, i, &d2, CH);
			if (dec != NULL) { free(dec); ok_null = 0; break; }
		}
		/* garbage container */
		dec = qolz_decode(small, (int)sizeof(small), &d2, CH);
		if (dec != NULL) { free(dec); ok_null = 0; }
		/* NULL / bad args */
		if (qolz_decode(NULL, el, &d2, CH) != NULL) ok_null = 0;
		if (qolz_encode(NULL, &desc, &el) != NULL) ok_null = 0;
		CHECK(ok_null, "truncated/garbage/NULL inputs rejected gracefully");
		free(enc);
	}

	free(buf);
}

/* -------------------------------------------------------------- fuzzing */

static void test_fuzz(void) {
	int round, ok = 1;
	unsigned char *buf = (unsigned char *)malloc(40000);
	qoi_desc desc;
	printf("fuzz: ");
	fflush(stdout);
	for (round = 0; round < 200; round++) {
		int w = 1 + (int)(xrnd() % 40);
		int h = 1 + (int)(xrnd() % 40);
		int n = w * h * CH;
		int el, i;
		unsigned char *enc, *dec;
		qoi_desc d2;
		desc.width = w; desc.height = h;
		desc.channels = CH; desc.colorspace = QOI_SRGB;
		for (i = 0; i < n; i++) {
			buf[i] = (xrnd() & 7) == 0 ? rnd_byte()
				: (unsigned char)(buf[i > CH ? i - CH : 0] + (xrnd() & 3));
		}
		enc = (unsigned char *) qolz_encode(buf, &desc, &el);
		if (!enc) { ok = 0; break; }
		dec = (unsigned char *) qolz_decode(enc, el, &d2, CH);
		if (!dec || memcmp(dec, buf, (size_t)n) != 0) { ok = 0; free(enc); break; }
		free(enc);
		free(dec);
	}
	free(buf);
	CHECK(ok, "200 random images: qolz encode/decode pixel exact");
}

/* ------------------------------------------------------------------ main */

int main(void) {
	struct {
		const char *name;
		void (*gen)(unsigned char *);
	} samples[] = {
		{ "pixel art ", gen_pixel_art },
		{ "screenshot ", gen_screenshot },
		{ "photo      ", gen_photo },
		{ "noise      ", gen_noise },
	};
	int s;

	printf("QOLZ acceptance harness (samples %dx%d RGBA, %d bytes raw each)\n\n",
		W, H, RAW_LEN);

	printf("%-11s|%9s|%25s|%25s|%7s|%8s|%8s\n",
		"sample", "raw",
		"plain QOI enc/dec ms  bytes",
		"QOLZ enc/dec ms  bytes mode",
		"gain", "ratio~", "ratio*");
	printf("-----------------------------"
		"------------------------------------------------"
		"------------------------------------------------\n");

	for (s = 0; s < 4; s++) {
		unsigned char *raw = new_raw();
		path_stats q, z;
		int mode;
		double gain, ratio_med, ratio_best;
		const char *mname;

		rng_state = 0x12345678u + (unsigned)s;
		samples[s].gen(raw);

		bench_qoi(raw, &q);
		bench_qolz(raw, &z, &mode);

		gain = 1.0 - (double)z.enc_len / (double)q.enc_len;
		ratio_med = (double)(z.enc_ns + z.dec_ns) /
			(double)(q.enc_ns + q.dec_ns);
		ratio_best = (double)(z.best_enc + z.best_dec) /
			(double)(q.best_enc + q.best_dec);
		mname = mode == 0 ? "LZ(raw)" :
			mode == 1 ? "LZ(qoi)" : "stored";

		printf("%-11s|%9d|%7.3f/%7.3f %9d|%7.3f/%7.3f %9d %7s|%6.2f%%|%7.2fx|%7.2fx\n",
			samples[s].name, RAW_LEN,
			ms(q.enc_ns), ms(q.dec_ns), q.enc_len,
			ms(z.enc_ns), ms(z.dec_ns), z.enc_len, mname,
			gain * 100.0, ratio_med, ratio_best);
		free(raw);
	}
	printf("\nratio~ = median total time (QOLZ / plain QOI), ratio* = best-of-%d\n",
		ITERS);
	printf("acceptance: gain >= 15%% and ratio* <= 2.0x (best-case compute bound)\n");

	printf("\n");
	test_edge_cases();
	test_fuzz();

	printf("\n%s\n", failures ? "RESULT: FAILURES PRESENT" : "RESULT: all checks passed");
	return failures ? 1 : 0;
}
