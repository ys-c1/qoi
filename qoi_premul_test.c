/*

qoi_premul_test.c

Test suite for the premultiplied-alpha auto-detection in qoi.h. Covers:

  - exhaustive proof that un-premultiply/re-premultiply is exactly
    reversible for all 0 <= r,g,b <= a <= 255 (lossless round trip)
  - the four asset classes: clearly premultiplied, clearly straight,
    large semi-transparent area, no alpha
  - detection accuracy on randomized sweeps of each class
  - misjudgment consequences (round trip stays byte-exact)
  - "original encoding unchanged": unflagged output is byte-identical to
    the unmodified qoi.h from git HEAD (qoi_orig_encode, see
    run_premul_tests.sh)
  - per-class encode/decode timing

If argv[1] is given, the four class samples (plus a 3-channel opaque
sample) are written there as .qoi files for qoiconv/qoibench/qoifuzz.

Build (from the repo root, see run_premul_tests.sh):
    gcc -std=c99 -O2 -Wall -Wextra qoi_premul_test.c qoi_orig.o -o qoi_premul_test

*/

#define _POSIX_C_SOURCE 199309L

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define QOI_IMPLEMENTATION
#include "qoi.h"

/* The unmodified qoi.h from git HEAD, compiled with renamed symbols. */
void *qoi_orig_encode(const void *data, const qoi_desc *desc, int *out_len);
void *qoi_orig_decode(const void *data, int size, qoi_desc *desc, int channels);

static int failures = 0;

#define CHECK(cond, ...) do { \
		if (!(cond)) { \
			failures++; \
			printf("FAIL %s:%d: ", __FILE__, __LINE__); \
			printf(__VA_ARGS__); \
			printf("\n"); \
		} \
	} while (0)

static uint64_t now_ns(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint64_t rng_state = 0x9e3779b97f4a7c15ull;
static uint32_t rnd(void) {
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 7;
	rng_state ^= rng_state << 17;
	return (uint32_t)(rng_state & 0xffffffffu);
}

/* ---------------------------------------------------------------- image
generators for the four asset classes. All are small and generated in
memory. */

/* class 1: clearly premultiplied sprite, rgb = round(base * a / 255) */
static void gen_premul(unsigned char *px, int w, int h, int *edge_out) {
	int x, y, edge = 0;
	for (y = 0; y < h; y++) {
		for (x = 0; x < w; x++) {
			unsigned char *p = px + (y * w + x) * 4;
			unsigned char a = (unsigned char)(((x + y) * 255) / (w + h - 2));
			p[0] = qoi_premultiply_channel(201, a);
			p[1] = qoi_premultiply_channel(140, a);
			p[2] = qoi_premultiply_channel(77, a);
			p[3] = a;
			edge += (a > 0 && a < 255);
		}
	}
	*edge_out = edge;
}

/* class 2: clearly straight sprite, same alpha field, rgb independent of a */
static void gen_straight(unsigned char *px, int w, int h) {
	int x, y;
	for (y = 0; y < h; y++) {
		for (x = 0; x < w; x++) {
			unsigned char *p = px + (y * w + x) * 4;
			p[0] = 201;
			p[1] = 140;
			p[2] = 77;
			p[3] = (unsigned char)(((x + y) * 255) / (w + h - 2));
		}
	}
}

/* class 3: large semi-transparent area, all rgb <= a (ambiguous: looks
premultiplied even if authored straight) */
static void gen_slab(unsigned char *px, int w, int h) {
	int i, n = w * h;
	for (i = 0; i < n; i++) {
		px[i * 4 + 0] = 64;
		px[i * 4 + 1] = 100;
		px[i * 4 + 2] = 120;
		px[i * 4 + 3] = 128;
	}
}

/* class 4: no alpha (fully opaque) */
static void gen_opaque(unsigned char *px, int w, int h, int channels) {
	int i, n = w * h * channels;
	for (i = 0; i < n; i++) {
		px[i] = (unsigned char)rnd();
	}
	if (channels == 4) {
		for (i = 0; i < w * h; i++) {
			px[i * 4 + 3] = 255;
		}
	}
}

/* compressible variant for the on-disk samples: qoibench's libpng wrapper
aborts when a PNG is larger than the raw pixels, which random noise is */
static void gen_opaque_smooth(unsigned char *px, int w, int h, int channels) {
	int x, y;
	for (y = 0; y < h; y++) {
		for (x = 0; x < w; x++) {
			unsigned char *p = px + (y * w + x) * channels;
			p[0] = (unsigned char)(x * 4);
			p[1] = (unsigned char)(y * 4);
			p[2] = (unsigned char)((x + y) * 2);
			if (channels == 4) {
				p[3] = 255;
			}
		}
	}
}

/* ------------------------------------------------------------------ */

static void bench(const char *name, const unsigned char *px, int w, int h, int channels) {
	qoi_desc desc = { (unsigned)w, (unsigned)h, (unsigned char)channels, QOI_SRGB };
	qoi_desc dd;
	int len = 0, reps = (int)(60000000 / ((unsigned)w * h));
	uint64_t t0, t_enc, t_enc_orig, t_dec;
	void *enc, *dec;
	int i;

	if (reps < 20) { reps = 20; }
	if (reps > 20000) { reps = 20000; }

	t0 = now_ns();
	for (i = 0; i < reps; i++) {
		enc = qoi_encode(px, &desc, &len);
		free(enc);
	}
	t_enc = now_ns() - t0;

	t0 = now_ns();
	for (i = 0; i < reps; i++) {
		enc = qoi_orig_encode(px, &desc, &len);
		free(enc);
	}
	t_enc_orig = now_ns() - t0;

	enc = qoi_encode(px, &desc, &len);
	t0 = now_ns();
	for (i = 0; i < reps; i++) {
		dec = qoi_decode(enc, len, &dd, 0);
		free(dec);
	}
	t_dec = now_ns() - t0;
	free(enc);

	printf(
		"  %-28s encode %7.1f ns/px (orig %7.1f)  decode %7.1f ns/px\n",
		name,
		(double)t_enc / ((double)reps * w * h),
		(double)t_enc_orig / ((double)reps * w * h),
		(double)t_dec / ((double)reps * w * h)
	);
}

/* Encode one image, verify flag/round-trip/byte-identity, return flag. */
static int check_image(
	const char *name, const unsigned char *px, int w, int h, int channels,
	int expect_flag
) {
	qoi_desc desc = { (unsigned)w, (unsigned)h, (unsigned char)channels, QOI_SRGB };
	qoi_desc d2, d3;
	int enc_len = 0, orig_len = 0;
	void *enc = qoi_encode(px, &desc, &enc_len);
	void *orig = qoi_orig_encode(px, &desc, &orig_len);
	void *dec, *dec3;
	int flagged;

	CHECK(enc != NULL, "%s: qoi_encode failed", name);
	CHECK(orig != NULL, "%s: qoi_orig_encode failed", name);
	if (!enc || !orig) { free(enc); free(orig); return -1; }

	flagged = !!(((unsigned char *)enc)[13] & QOI_PREMULTIPLIED);
	CHECK(flagged == expect_flag,
		"%s: QOI_PREMULTIPLIED flag = %d, expected %d (threshold %d)",
		name, flagged, expect_flag, QOI_PREMULTIPLY_MIN_EDGE);

	/* round trip must restore the original pixels byte for byte */
	dec = qoi_decode(enc, enc_len, &d2, 0);
	CHECK(dec != NULL, "%s: qoi_decode failed", name);
	CHECK(dec && memcmp(dec, px, (size_t)w * h * channels) == 0,
		"%s: round trip is not byte-exact", name);
	CHECK(d2.colorspace == QOI_SRGB,
		"%s: decoded colorspace %d, flag not masked out", name, d2.colorspace);
	free(dec);

	if (!flagged) {
		/* original encoding must be unchanged, byte for byte */
		CHECK(enc_len == orig_len && memcmp(enc, orig, enc_len) == 0,
			"%s: unflagged encoding differs from original qoi.h", name);
	}
	else {
		/* interop note: the strict reference decoder rejects flagged files */
		dec = qoi_orig_decode(enc, enc_len, &d3, 0);
		printf("  note: %s: stock qoi.h decoder %s the flagged file\n",
			name, dec ? "accepts" : "rejects (unknown colorspace)");
		free(dec);

		if (channels == 4) {
			/* decoding to RGB must restore the premultiplied RGB */
			int i, ok = 1;
			dec3 = qoi_decode(enc, enc_len, &d3, 3);
			CHECK(dec3 != NULL, "%s: qoi_decode to RGB failed", name);
			for (i = 0; dec3 && i < w * h; i++) {
				ok &= dec3 && ((unsigned char *)dec3)[i * 3 + 0] == px[i * 4 + 0]
					&& ((unsigned char *)dec3)[i * 3 + 1] == px[i * 4 + 1]
					&& ((unsigned char *)dec3)[i * 3 + 2] == px[i * 4 + 2];
			}
			CHECK(ok, "%s: RGB decode did not restore premultiplied RGB", name);
			free(dec3);
		}
	}

	free(enc);
	free(orig);
	return flagged;
}

int main(int argc, char **argv) {
	int a, p, edge, i;
	unsigned char *img;
	int f_premul, f_straight, f_slab, f_opaque4, f_opaque3;

	printf("qoi premultiplied-alpha test, QOI_PREMULTIPLY_MIN_EDGE=%d\n",
		QOI_PREMULTIPLY_MIN_EDGE);

	/* ---- exhaustive inverse proof over the whole valid domain ---- */
	{
		int bad = 0;
		for (a = 0; a <= 255; a++) {
			for (p = 0; p <= a; p++) {
				bad += qoi_premultiply_channel(qoi_unpremultiply_channel(p, a), a) != p;
			}
		}
		CHECK(bad == 0, "un-premultiply/re-premultiply not reversible (%d cases)", bad);
		printf("inverse check: all %d (p,a) pairs with p<=a round-trip exactly\n",
			256 * 257 / 2);
	}

	/* ---- the four asset classes ---- */
	img = malloc(128 * 128 * 4);

	gen_premul(img, 64, 64, &edge);
	f_premul = check_image("class1 clearly-premultiplied 64x64", img, 64, 64, 4,
		edge >= QOI_PREMULTIPLY_MIN_EDGE);

	gen_straight(img, 64, 64);
	f_straight = check_image("class2 clearly-straight 64x64", img, 64, 64, 4, 0);

	gen_slab(img, 128, 128);
	f_slab = check_image("class3 semi-transparent-slab 128x128", img, 128, 128, 4,
		128 * 128 >= QOI_PREMULTIPLY_MIN_EDGE);

	gen_opaque(img, 64, 64, 4);
	f_opaque4 = check_image("class4 no-alpha RGBA 64x64", img, 64, 64, 4, 0);

	gen_opaque(img, 64, 64, 3);
	f_opaque3 = check_image("class4 no-alpha RGB 64x64", img, 64, 64, 3, 0);

	printf("flags: premul=%d straight=%d slab=%d opaque4=%d opaque3=%d\n",
		f_premul, f_straight, f_slab, f_opaque4, f_opaque3);

	/* ---- randomized accuracy sweep, 64 images per class ---- */
	{
		int n = 64, w = 32, h = 32, np = w * h;
		int c1_ok = 0, c2_ok = 0, c3_flagged = 0, c3_exact = 0, c4_ok = 0;
		for (i = 0; i < n; i++) {
			int j, e, violations, flagged;
			qoi_desc desc = { (unsigned)w, (unsigned)h, 4, QOI_SRGB };
			qoi_desc d2;
			int el = 0, ol = 0;
			void *enc, *orig, *dec;

			/* random premultiplied: rgb = round(s*a/255) <= a */
			e = 0;
			for (j = 0; j < np; j++) {
				unsigned char aa = (unsigned char)rnd();
				img[j * 4 + 0] = qoi_premultiply_channel((unsigned char)rnd(), aa);
				img[j * 4 + 1] = qoi_premultiply_channel((unsigned char)rnd(), aa);
				img[j * 4 + 2] = qoi_premultiply_channel((unsigned char)rnd(), aa);
				img[j * 4 + 3] = aa;
				e += (aa > 0 && aa < 255);
			}
			enc = qoi_encode(img, &desc, &el);
			flagged = !!(((unsigned char *)enc)[13] & QOI_PREMULTIPLIED);
			dec = qoi_decode(enc, el, &d2, 0);
			c1_ok += flagged == (e >= QOI_PREMULTIPLY_MIN_EDGE)
				&& dec && memcmp(dec, img, np * 4) == 0;
			free(enc); free(dec);

			/* random straight: fully random rgba, force one violation */
			violations = 0;
			for (j = 0; j < np; j++) {
				img[j * 4 + 0] = (unsigned char)rnd();
				img[j * 4 + 1] = (unsigned char)rnd();
				img[j * 4 + 2] = (unsigned char)rnd();
				img[j * 4 + 3] = (unsigned char)rnd();
			}
			img[0] = 255; img[3] = 0; /* guaranteed straight evidence */
			for (j = 0; j < np; j++) {
				unsigned char aa = img[j * 4 + 3];
				violations += img[j*4] > aa || img[j*4+1] > aa || img[j*4+2] > aa;
			}
			enc = qoi_encode(img, &desc, &el);
			orig = qoi_orig_encode(img, &desc, &ol);
			flagged = !!(((unsigned char *)enc)[13] & QOI_PREMULTIPLIED);
			dec = qoi_decode(enc, el, &d2, 0);
			c2_ok += violations > 0 && !flagged
				&& el == ol && memcmp(enc, orig, el) == 0
				&& dec && memcmp(dec, img, np * 4) == 0;
			free(enc); free(orig); free(dec);

			/* random ambiguous slab: straight-authored, all rgb <= a */
			for (j = 0; j < np; j++) {
				unsigned char aa = 1 + (unsigned char)(rnd() % 254);
				img[j * 4 + 0] = (unsigned char)(rnd() % (aa + 1));
				img[j * 4 + 1] = (unsigned char)(rnd() % (aa + 1));
				img[j * 4 + 2] = (unsigned char)(rnd() % (aa + 1));
				img[j * 4 + 3] = aa;
			}
			enc = qoi_encode(img, &desc, &el);
			flagged = !!(((unsigned char *)enc)[13] & QOI_PREMULTIPLIED);
			dec = qoi_decode(enc, el, &d2, 0);
			c3_flagged += flagged;
			c3_exact += dec && memcmp(dec, img, np * 4) == 0;
			free(enc); free(dec);

			/* random opaque */
			gen_opaque(img, w, h, 4);
			enc = qoi_encode(img, &desc, &el);
			orig = qoi_orig_encode(img, &desc, &ol);
			flagged = !!(((unsigned char *)enc)[13] & QOI_PREMULTIPLIED);
			c4_ok += !flagged && el == ol && memcmp(enc, orig, el) == 0;
			free(enc); free(orig);
		}

		printf("\naccuracy over %d random images per class (threshold %d):\n",
			n, QOI_PREMULTIPLY_MIN_EDGE);
		printf("  class1 premultiplied:        %d/%d correct\n", c1_ok, n);
		printf("  class2 straight:             %d/%d correct (byte-identical to orig)\n", c2_ok, n);
		printf("  class3 semi-transparent:     %d/%d flagged premultiplied"
			" (ambiguous class), %d/%d still byte-exact after round trip\n",
			c3_flagged, n, c3_exact, n);
		printf("  class4 no alpha:             %d/%d correct (byte-identical to orig)\n", c4_ok, n);

		CHECK(c1_ok == n, "sweep: premultiplied detection inaccurate");
		CHECK(c2_ok == n, "sweep: straight detection inaccurate");
		CHECK(c3_exact == n, "sweep: ambiguous-class round trip lossy");
		CHECK(c4_ok == n, "sweep: opaque detection inaccurate");
	}

	/* ---- timing ---- */
	printf("\ntiming (single thread, this machine):\n");
	gen_premul(img, 64, 64, &edge);
	bench("class1 premultiplied 64x64", img, 64, 64, 4);
	gen_straight(img, 64, 64);
	bench("class2 straight 64x64", img, 64, 64, 4);
	gen_slab(img, 128, 128);
	bench("class3 slab 128x128", img, 128, 128, 4);
	gen_opaque(img, 64, 64, 4);
	bench("class4 opaque RGBA 64x64", img, 64, 64, 4);
	gen_opaque(img, 64, 64, 3);
	bench("class4 opaque RGB 64x64", img, 64, 64, 3);

	/* ---- optional: dump samples for qoiconv/qoibench/qoifuzz ---- */
	if (argc > 1) {
		char path[1024];
		struct { const char *name; int w, h, ch; } spec[5] = {
			{ "class1_premul", 64, 64, 4 },
			{ "class2_straight", 64, 64, 4 },
			{ "class3_slab", 128, 128, 4 },
			{ "class4_opaque_rgba", 64, 64, 4 },
			{ "class4_opaque_rgb", 64, 64, 3 },
		};
		for (i = 0; i < 5; i++) {
			qoi_desc desc = {
				(unsigned)spec[i].w, (unsigned)spec[i].h,
				(unsigned char)spec[i].ch, QOI_SRGB
			};
			int e2;
			snprintf(path, sizeof(path), "%s/%s.qoi", argv[1], spec[i].name);
			switch (i) {
				case 0: gen_premul(img, spec[i].w, spec[i].h, &e2); break;
				case 1: gen_straight(img, spec[i].w, spec[i].h); break;
				case 2: gen_slab(img, spec[i].w, spec[i].h); break;
				case 3: gen_opaque_smooth(img, spec[i].w, spec[i].h, 4); break;
				default: gen_opaque_smooth(img, spec[i].w, spec[i].h, 3); break;
			}
			CHECK(qoi_write(path, img, &desc) > 0, "cannot write %s", path);
		}
		printf("samples written to %s\n", argv[1]);
	}

	free(img);

	if (failures) {
		printf("\n%d CHECK(S) FAILED\n", failures);
		return 1;
	}
	printf("\nall checks passed\n");
	return 0;
}
