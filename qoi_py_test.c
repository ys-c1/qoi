/*

Layered (progressive pyramid) codec test + micro-benchmark for qoi.h.

Self contained: all sample images are synthesized in code, no files, no
third-party libraries, no network. Compile with:

	gcc -std=c99 -O3 -Wall -Wextra qoi_py_test.c -o qoi_py_test

Exits non-zero if any check fails.

*/

#define _POSIX_C_SOURCE 199309L
#define QOI_IMPLEMENTATION
#include "qoi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

static int g_failures = 0;

#define CHECK(cond, ...) do { \
	if (!(cond)) { \
		g_failures++; \
		printf("  FAIL @line %d: ", __LINE__); \
		printf(__VA_ARGS__); \
		printf("\n"); \
	} \
} while (0)

static uint64_t now_ns(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

#define SAMPLE_W 64
#define SAMPLE_H 64
#define SAMPLE_CH 4
#define LAYERS 3
#define TIMING_ITERS 2000

typedef struct {
	const char *name;
	unsigned char pixels[SAMPLE_W * SAMPLE_H * SAMPLE_CH];
} sample_t;

static void px(sample_t *s, int x, int y, int r, int g, int b, int a) {
	unsigned char *p = s->pixels + (y * SAMPLE_W + x) * SAMPLE_CH;
	p[0] = (unsigned char)r; p[1] = (unsigned char)g;
	p[2] = (unsigned char)b; p[3] = (unsigned char)a;
}

/* 1. Smooth gradient -- low-frequency content, compresses very well */
static void gen_gradient(sample_t *s) {
	for (int y = 0; y < SAMPLE_H; y++) {
		for (int x = 0; x < SAMPLE_W; x++) {
			px(s, x, y, x * 4, y * 4, (x + y) * 2, 255);
		}
	}
}

/* 2. Flat UI icon -- large flat areas + a sharp border and alpha shapes */
static void gen_icon(sample_t *s) {
	memset(s->pixels, 0, sizeof(s->pixels)); /* fully transparent bg */
	for (int y = 8; y < 56; y++) {
		for (int x = 8; x < 56; x++) {
			int border = (x < 11 || x > 52 || y < 11 || y > 52);
			int disk = ((x - 32) * (x - 32) + (y - 32) * (y - 32)) < 144;
			if (disk) {
				px(s, x, y, 40, 120, 220, 255);
			} else if (border) {
				px(s, x, y, 30, 30, 30, 255);
			} else {
				px(s, x, y, 230, 235, 240, 255);
			}
		}
	}
}

/* 3. High-frequency noise texture -- residuals stay large */
static void gen_noise(sample_t *s) {
	unsigned int seed = 0x12345678u;
	for (int i = 0; i < SAMPLE_W * SAMPLE_H; i++) {
		seed = seed * 1103515245u + 12345u;
		unsigned char r = (unsigned char)(seed >> 16);
		seed = seed * 1103515245u + 12345u;
		unsigned char g = (unsigned char)(seed >> 16);
		seed = seed * 1103515245u + 12345u;
		unsigned char b = (unsigned char)(seed >> 16);
		px(s, i % SAMPLE_W, i / SAMPLE_W, r, g, b, 255);
	}
}

/* 4. Line art / text-like material -- hard edges, two colors, alpha */
static void gen_lineart(sample_t *s) {
	for (int y = 0; y < SAMPLE_H; y++) {
		for (int x = 0; x < SAMPLE_W; x++) {
			int on = ((x / 4 + y / 4) % 2 == 0) || (x == y) || (x + y == 63);
			if (on) {
				px(s, x, y, 10, 10, 10, 255);
			} else {
				px(s, x, y, 250, 250, 250, (x < 32) ? 255 : 180);
			}
		}
	}
}

/* Read a stored layer's byte length straight from the container header's
per-layer length table. Returns 0 if the index is unused. */
static int layer_length(const unsigned char *data, int li) {
	/* fixed fields occupy QOI_PY_FIXED_SIZE bytes; then BE uint32 lengths */
	int p = QOI_PY_FIXED_SIZE + li * 4;
	return (data[p] << 24) | (data[p + 1] << 16) |
	       (data[p + 2] << 8) | data[p + 3];
}

static const char *kernel_name(int k) {
	return k == QOI_PY_KERNEL_BOX ? "box"
		: k == QOI_PY_KERNEL_TENT ? "tent" : "lanczos2";
}

static double time_decode_layer0(const unsigned char *enc, int enc_len, int iters) {
	uint64_t best = UINT64_MAX;
	for (int round = 0; round < 5; round++) {
		uint64_t t0 = now_ns();
		for (int i = 0; i < iters; i++) {
			qoi_py_desc d;
			void *p = qoi_py_decode(enc, enc_len, &d, 1);
			if (!p) { return -1.0; }
			free(p);
		}
		uint64_t dt = now_ns() - t0;
		if (dt < best) { best = dt; }
	}
	return (double)best / (double)iters / 1000.0; /* microseconds */
}

static double time_decode_full(const unsigned char *enc, int enc_len, int iters) {
	uint64_t best = UINT64_MAX;
	for (int round = 0; round < 5; round++) {
		uint64_t t0 = now_ns();
		for (int i = 0; i < iters; i++) {
			qoi_py_desc d;
			void *p = qoi_py_decode(enc, enc_len, &d, LAYERS);
			if (!p) { return -1.0; }
			free(p);
		}
		uint64_t dt = now_ns() - t0;
		if (dt < best) { best = dt; }
	}
	return (double)best / (double)iters / 1000.0;
}

static double time_orig_qoi_decode(const unsigned char *enc, int enc_len, int iters) {
	uint64_t best = UINT64_MAX;
	for (int round = 0; round < 5; round++) {
		uint64_t t0 = now_ns();
		for (int i = 0; i < iters; i++) {
			qoi_desc d;
			void *p = qoi_decode(enc, enc_len, &d, 4);
			if (!p) { return -1.0; }
			free(p);
		}
		uint64_t dt = now_ns() - t0;
		if (dt < best) { best = dt; }
	}
	return (double)best / (double)iters / 1000.0;
}

int main(void) {
	sample_t samples[4];
	memset(samples, 0, sizeof(samples));
	samples[0].name = "gradient (smooth)";   gen_gradient(&samples[0]);
	samples[1].name = "ui-icon (flat+alpha)"; gen_icon(&samples[1]);
	samples[2].name = "noise (high-freq)";   gen_noise(&samples[2]);
	samples[3].name = "line-art (edges)";    gen_lineart(&samples[3]);

	const int kernels[3] = {
		QOI_PY_KERNEL_BOX, QOI_PY_KERNEL_TENT, QOI_PY_KERNEL_LANCZOS2
	};

	printf("== qoi layered codec: functional checks ==\n");

	/* --- layer_count 0 must error, for every kernel and both APIs --- */
	for (int k = 0; k < 3; k++) {
		qoi_py_desc d0 = {
			SAMPLE_W, SAMPLE_H, SAMPLE_CH, QOI_SRGB, 0,
			(unsigned char)kernels[k]
		};
		int len = 123;
		void *r = qoi_py_encode(samples[0].pixels, &d0, &len);
		CHECK(r == NULL, "qoi_py_encode with 0 layers must fail (%s)",
			kernel_name(kernels[k]));

		qoi_py_desc good = {
			SAMPLE_W, SAMPLE_H, SAMPLE_CH, QOI_SRGB, LAYERS,
			(unsigned char)kernels[k]
		};
		void *enc = qoi_py_encode(samples[0].pixels, &good, &len);
		CHECK(enc != NULL, "encode for decode-0 test failed");
		if (enc) {
			qoi_py_desc dd;
			CHECK(qoi_py_decode(enc, len, &dd, 0) == NULL,
				"qoi_py_decode with 0 layers must fail (%s)",
				kernel_name(kernels[k]));
			free(enc);
		}
	}

	/* --- invalid params / truncated containers must fail safely --- */
	{
		qoi_py_desc d = {
			SAMPLE_W, SAMPLE_H, SAMPLE_CH, QOI_SRGB, LAYERS,
			QOI_PY_KERNEL_BOX
		};
		int len;
		void *enc = qoi_py_encode(samples[0].pixels, &d, &len);
		CHECK(enc != NULL, "encode failed");
		if (enc) {
			qoi_py_desc dd;
			CHECK(qoi_py_decode(NULL, len, &dd, 1) == NULL, "null data");
			CHECK(qoi_py_decode(enc, 0, &dd, 1) == NULL, "zero size");
			CHECK(qoi_py_decode(enc, len, &dd, LAYERS + 1) == NULL,
				"decode_layers > layer_count");
			void *tr = qoi_py_decode(enc, len / 2, &dd, LAYERS);
			CHECK(tr == NULL, "truncated container must fail full decode");
			/* A prefix holding exactly the header + layer-0 stream must still
			yield the thumbnail, proving layer 0 decodes without later bytes. */
			int prefix_len = QOI_PY_HEADER_SIZE + layer_length(enc, 0);
			void *thumb = qoi_py_decode(enc, prefix_len, &dd, 1);
			CHECK(thumb != NULL && dd.width == 16 && dd.height == 16,
				"thumbnail decode from layer-0-only prefix should work");
			/* ...but the same prefix cannot satisfy a 2-layer request */
			CHECK(qoi_py_decode(enc, prefix_len, &dd, 2) == NULL,
				"prefix missing layer 1 must fail a 2-layer decode");
			free(thumb);
			free(enc);
		}
	}

	/* --- every sample x kernel: all partial decodes + exact full decode --- */
	for (int si = 0; si < 4; si++) {
		for (int ki = 0; ki < 3; ki++) {
			qoi_py_desc d = {
				SAMPLE_W, SAMPLE_H, SAMPLE_CH, QOI_SRGB, LAYERS,
				(unsigned char)kernels[ki]
			};
			int enc_len;
			unsigned char *enc = qoi_py_encode(samples[si].pixels, &d, &enc_len);
			CHECK(enc != NULL, "encode %s/%s failed",
				samples[si].name, kernel_name(kernels[ki]));
			if (!enc) { continue; }

			/* expected resolutions: 16x16, 32x32, 64x64 */
			const int ew[3] = {16, 32, 64};
			const int eh[3] = {16, 32, 64};
			for (int dl = 1; dl <= LAYERS; dl++) {
				qoi_py_desc dd;
				unsigned char *out = qoi_py_decode(enc, enc_len, &dd, dl);
				CHECK(out != NULL, "decode l%d %s/%s failed",
					dl, samples[si].name, kernel_name(kernels[ki]));
				if (out) {
					CHECK(dd.width == (unsigned)ew[dl - 1] &&
					      dd.height == (unsigned)eh[dl - 1],
						"layer %d resolution %ux%u != %dx%d",
						dl, dd.width, dd.height, ew[dl - 1], eh[dl - 1]);
					CHECK(dd.layer_count == LAYERS && dd.kernel == kernels[ki],
						"header roundtrip mismatch");
					free(out);
				}
			}

			/* full decode must equal the source byte for byte */
			qoi_py_desc dd;
			unsigned char *full = qoi_py_decode(enc, enc_len, &dd, LAYERS);
			CHECK(full != NULL, "full decode failed");
			if (full) {
				int mismatch = memcmp(full, samples[si].pixels,
					SAMPLE_W * SAMPLE_H * SAMPLE_CH);
				CHECK(mismatch == 0,
					"pixel-exact reconstruction failed for %s/%s",
					samples[si].name, kernel_name(kernels[ki]));
				free(full);
			}
			free(enc);
		}
	}

	/* --- original single-layer codec output must be byte-for-byte stable --- */
	printf("\n== original qoi_encode byte stability ==\n");
	for (int si = 0; si < 4; si++) {
		qoi_desc d = {
			SAMPLE_W, SAMPLE_H, SAMPLE_CH, QOI_SRGB
		};
		int l1, l2;
		void *e1 = qoi_encode(samples[si].pixels, &d, &l1);
		void *e2 = qoi_encode(samples[si].pixels, &d, &l2);
		CHECK(e1 && e2 && l1 == l2 && memcmp(e1, e2, l1) == 0,
			"qoi_encode not deterministic");
		/* reference: hand-check the magic and end marker framing */
		CHECK(e1 && ((unsigned char *)e1)[0] == 'q' &&
		      ((unsigned char *)e1)[3] == 'f', "qoi magic");
		CHECK(e1 && ((unsigned char *)e1)[l1 - 1] == 1 &&
		      ((unsigned char *)e1)[l1 - 8] == 0, "qoi end marker");
		qoi_desc rd;
		void *dec = qoi_decode(e1, l1, &rd, 4);
		CHECK(dec && memcmp(dec, samples[si].pixels,
			SAMPLE_W * SAMPLE_H * 4) == 0, "qoi roundtrip");
		free(dec); free(e1); free(e2);
	}

	/* --- metrics per sample (box kernel used for the headline numbers) --- */
	printf("\n== metrics (layers=%d, 64x64 RGBA, kernel=box) ==\n", LAYERS);
	printf("%-22s %10s %10s %10s | %10s %10s %10s | %10s\n",
		"sample", "L0 bytes", "L1 bytes", "L2 bytes",
		"L0 share", "L1 share", "L2 share", "total");
	for (int si = 0; si < 4; si++) {
		qoi_py_desc d = {
			SAMPLE_W, SAMPLE_H, SAMPLE_CH, QOI_SRGB, LAYERS,
			QOI_PY_KERNEL_BOX
		};
		int enc_len;
		unsigned char *enc = qoi_py_encode(samples[si].pixels, &d, &enc_len);
		CHECK(enc != NULL, "metrics encode failed");
		if (!enc) { continue; }
		int len0 = layer_length(enc, 0);
		int len1 = layer_length(enc, 1);
		int len2 = layer_length(enc, 2);
		CHECK(len0 > 0 && len1 > 0 && len2 > 0, "layer length table");
		int payload = len0 + len1 + len2;
		printf("%-22s %10d %10d %10d | %9.2f%% %9.2f%% %9.2f%% | %10d\n",
			samples[si].name, len0, len1, len2,
			100.0 * len0 / payload, 100.0 * len1 / payload,
			100.0 * len2 / payload, enc_len);
		free(enc);
	}

	/* --- timing + exactness across kernels --- */
	printf("\n== timing: best-of-5, %d iters each, microseconds/call ==\n",
		TIMING_ITERS);
	printf("%-22s %-8s %12s %12s %14s %10s\n",
		"sample", "kernel", "decode L0", "decode all",
		"orig qoi dec", "exact?");
	for (int si = 0; si < 4; si++) {
		for (int ki = 0; ki < 3; ki++) {
			qoi_py_desc d = {
				SAMPLE_W, SAMPLE_H, SAMPLE_CH, QOI_SRGB, LAYERS,
				(unsigned char)kernels[ki]
			};
			int enc_len;
			unsigned char *enc = qoi_py_encode(samples[si].pixels, &d, &enc_len);
			qoi_desc od = { SAMPLE_W, SAMPLE_H, SAMPLE_CH, QOI_SRGB };
			int orig_len;
			unsigned char *orig = qoi_encode(samples[si].pixels, &od, &orig_len);
			CHECK(enc && orig, "timing encode failed");
			if (!enc || !orig) { continue; }

			double t0 = time_decode_layer0(enc, enc_len, TIMING_ITERS);
			double tf = time_decode_full(enc, enc_len, TIMING_ITERS);
			double to = time_orig_qoi_decode(orig, orig_len, TIMING_ITERS);

			qoi_py_desc dd;
			unsigned char *full = qoi_py_decode(enc, enc_len, &dd, LAYERS);
			int exact = full && memcmp(full, samples[si].pixels,
				SAMPLE_W * SAMPLE_H * 4) == 0;
			free(full);

			printf("%-22s %-8s %12.3f %12.3f %14.3f %10s\n",
				samples[si].name, kernel_name(kernels[ki]),
				t0, tf, to, exact ? "yes" : "NO");
			CHECK(exact, "exactness failed in timing table");
			free(enc);
			free(orig);
		}
	}

	/* --- kernel configurability: different kernels, all still exact --- */
	printf("\n== kernel configurability check ==\n");
	for (int ki = 0; ki < 3; ki++) {
		qoi_py_desc d = {
			SAMPLE_W, SAMPLE_H, SAMPLE_CH, QOI_SRGB, LAYERS,
			(unsigned char)kernels[ki]
		};
		int enc_len;
		unsigned char *enc = qoi_py_encode(samples[2].pixels, &d, &enc_len);
		qoi_py_desc dd;
		unsigned char *full = qoi_py_decode(enc, enc_len, &dd, LAYERS);
		int exact = full && memcmp(full, samples[2].pixels,
			SAMPLE_W * SAMPLE_H * 4) == 0;
		printf("  %-9s roundtrip exact: %s\n", kernel_name(kernels[ki]),
			exact ? "yes" : "NO");
		CHECK(exact, "kernel %s not exact", kernel_name(kernels[ki]));
		free(full);
		free(enc);
	}

	/* --- layer count configurability: 1, 2, 4 layers all exact --- */
	printf("\n== layer count configurability check ==\n");
	for (int lc = 1; lc <= 4; lc++) {
		qoi_py_desc d = {
			SAMPLE_W, SAMPLE_H, SAMPLE_CH, QOI_SRGB,
			(unsigned char)lc, QOI_PY_KERNEL_TENT
		};
		int enc_len;
		unsigned char *enc = qoi_py_encode(samples[1].pixels, &d, &enc_len);
		CHECK(enc != NULL, "lc=%d encode", lc);
		qoi_py_desc dd;
		unsigned char *full = enc ? qoi_py_decode(enc, enc_len, &dd, lc) : NULL;
		int exact = full && dd.width == SAMPLE_W && dd.height == SAMPLE_H &&
			memcmp(full, samples[1].pixels, SAMPLE_W * SAMPLE_H * 4) == 0;
		printf("  layers=%d full-res exact: %s\n", lc, exact ? "yes" : "NO");
		CHECK(exact, "layer count %d not exact", lc);
		free(full);
		free(enc);
	}

	printf("\n==== %s ====\n", g_failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED");
	return g_failures ? 1 : 0;
}
