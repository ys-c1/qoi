/*

Near-lossless QOI test + benchmark harness.

Self contained: depends only on qoi.h and the C standard library. Generates
four small synthetic images (gradient, photo-like, pixel art, noise) and
exercises the qoi_encode_near() / qoi_decode_near() extension.

Compile (see run_tests.sh):
	gcc -std=c89 -pedantic -Wall -Wextra -O2 qoi_nl_test.c -o qoi_nl_test

Exit code is non-zero if any check fails.

*/

#define QOI_IMPLEMENTATION
#include "qoi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TEST_W 64
#define TEST_H 64
#define CHANNELS 4
#define NPIX (TEST_W * TEST_H)
#define NLEN (NPIX * CHANNELS)
#define ITERS 200

static int g_failures = 0;
static int g_checks = 0;

static void check(int cond, const char *name) {
	g_checks++;
	if (!cond) {
		g_failures++;
		printf("  [FAIL] %s\n", name);
	}
}

/* Deterministic xorshift32 PRNG so runs are reproducible. */
static unsigned int g_rng = 0x12345678u;
static unsigned int rnd(void) {
	unsigned int x = g_rng;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	g_rng = x;
	return x;
}

/* log10 without libm. Range-reduce x into [1,2) by counting factors of two,
evaluate ln on the small argument with the fast-converging atanh series
(y stays <= 1/3 there), then add back the powers of two. Only called with
x > 1 (finite MSE). */
static double log10_d(double x) {
	double ln_two_factor = 0.0;
	double y, y2, term, ln;
	int k;
	while (x >= 2.0) {
		x *= 0.5;
		ln_two_factor += 0.6931471805599453; /* ln(2) */
	}
	y = (x - 1.0) / (x + 1.0);
	y2 = y * y;
	term = y;
	ln = 0.0;
	for (k = 0; k < 40; k++) {
		ln += term / (2.0 * k + 1.0);
		term *= y2;
	}
	ln = 2.0 * ln + ln_two_factor;
	return ln / 2.302585092994046; /* ln(10) */
}

static double now_sec(void) {
	return (double)clock() / (double)CLOCKS_PER_SEC;
}

/* ---- Synthetic material generators (all 64x64 RGBA) -------------------- */

static void gen_gradient(unsigned char *px) {
	int x, y;
	for (y = 0; y < TEST_H; y++) {
		for (x = 0; x < TEST_W; x++) {
			unsigned char *p = px + (y * TEST_W + x) * CHANNELS;
			p[0] = (unsigned char)(x * 255 / (TEST_W - 1));
			p[1] = (unsigned char)(y * 255 / (TEST_H - 1));
			p[2] = (unsigned char)((x + y) * 255 / (TEST_W + TEST_H - 2));
			p[3] = 255;
		}
	}
}

static void gen_photo(unsigned char *px) {
	/* Smooth low-frequency fields + mild camera noise: compressible but with
	local variation, like a real photograph. */
	int x, y;
	for (y = 0; y < TEST_H; y++) {
		for (x = 0; x < TEST_W; x++) {
			unsigned char *p = px + (y * TEST_W + x) * CHANNELS;
			double fx = (double)x / TEST_W;
			double fy = (double)y / TEST_H;
			int r = (int)(128.0 + 80.0 * fx + 30.0 * fy) +
				(int)(rnd() % 9) - 4;
			int g = (int)(90.0 + 60.0 * fy + 40.0 * fx * fy) +
				(int)(rnd() % 9) - 4;
			int b = (int)(160.0 - 70.0 * fx + 35.0 * (1.0 - fy)) +
				(int)(rnd() % 9) - 4;
			p[0] = r < 0 ? 0 : (r > 255 ? 255 : (unsigned char)r);
			p[1] = g < 0 ? 0 : (g > 255 ? 255 : (unsigned char)g);
			p[2] = b < 0 ? 0 : (b > 255 ? 255 : (unsigned char)b);
			p[3] = 255;
		}
	}
}

static void gen_pixelart(unsigned char *px) {
	/* 8x8 flat cells in a hard-edged 4-color palette pattern. */
	static const unsigned char pal[4][4] = {
		{200, 60, 60, 255}, {60, 200, 90, 255},
		{50, 80, 210, 255}, {230, 210, 70, 255}
	};
	int x, y;
	for (y = 0; y < TEST_H; y++) {
		for (x = 0; x < TEST_W; x++) {
			int cx = x / 8;
			int cy = y / 8;
			int idx = ((cx * 7 + cy * 13) + ((cx ^ cy) & 1)) & 3;
			memcpy(px + (y * TEST_W + x) * CHANNELS, pal[idx], 4);
		}
	}
}

static void gen_noise(unsigned char *px) {
	int i;
	for (i = 0; i < NLEN; i++) {
		px[i] = (unsigned char)(rnd() & 0xff);
	}
	/* Keep alpha fully opaque so the material stays a normal photo-like
	stream; RGB carries the entropy. */
	for (i = 3; i < NLEN; i += 4) {
		px[i] = 255;
	}
}

/* ---- Metrics ------------------------------------------------------------- */

static void metrics(const unsigned char *a, const unsigned char *b,
	int max_abs_out[1], double *mse_out, double *psnr_out
) {
	long sum = 0;
	int max_abs = 0;
	int i;
	for (i = 0; i < NLEN; i++) {
		int d = (int)a[i] - (int)b[i];
		int ad = d < 0 ? -d : d;
		if (ad > max_abs) { max_abs = ad; }
		sum += (long)d * d;
	}
	*mse_out = (double)sum / (double)NLEN;
	*psnr_out = (*mse_out <= 0.0)
		? 999.0
		: 10.0 * log10_d((255.0 * 255.0) / *mse_out);
	*max_abs_out = max_abs;
}

static qoi_desc make_desc(void) {
	qoi_desc d;
	d.width = TEST_W;
	d.height = TEST_H;
	d.channels = CHANNELS;
	d.colorspace = QOI_SRGB;
	return d;
}

/* One benchmark row: encode/decode the material with given options. */
static void bench_row(const char *mat, const unsigned char *orig,
	int lossless_len, int tol, int dither, int rec
) {
	qoi_desc desc = make_desc();
	qoi_nl_desc nl;
	unsigned char *enc;
	int enc_len = 0;
	unsigned char *dec;
	qoi_desc ddesc;
	int max_abs;
	double mse, psnr;
	double t0, t1, enc_ms, dec_ms;
	int k;

	nl.tolerance = (unsigned char)tol;
	nl.dither = (unsigned char)dither;
	nl.record_error = (unsigned char)rec;

	t0 = now_sec();
	enc = NULL;
	for (k = 0; k < ITERS; k++) {
		unsigned char *cur =
			(unsigned char *)qoi_encode_near(orig, &desc, &nl, &enc_len);
		if (enc != NULL) { free(enc); }
		enc = cur;
	}
	t1 = now_sec();
	enc_ms = (t1 - t0) * 1000.0 / ITERS;
	check(enc != NULL, "encode returned non-NULL");

	t0 = now_sec();
	dec = NULL;
	for (k = 0; k < ITERS; k++) {
		unsigned char *cur = (unsigned char *)
			qoi_decode_near(enc, enc_len, &ddesc, CHANNELS);
		if (dec != NULL) { free(dec); }
		dec = cur;
	}
	t1 = now_sec();
	dec_ms = (t1 - t0) * 1000.0 / ITERS;
	check(dec != NULL, "decode returned non-NULL");
	check(ddesc.width == TEST_W && ddesc.height == TEST_H &&
		ddesc.channels == CHANNELS, "decoded descriptor matches");

	metrics(orig, dec, &max_abs, &mse, &psnr);

	printf("  %-9s t=%-3d dither=%d rec=%d | ratio=%6.3f | maxerr=%3d | "
		"PSNR=%7.2f dB | enc=%6.3f ms dec=%6.3f ms\n",
		mat, tol, dither, rec,
		(double)enc_len / (double)lossless_len,
		max_abs, psnr, enc_ms, dec_ms);

	free(dec);
	free(enc);
}

int main(void) {
	struct { const char *name; void (*gen)(unsigned char *); } mats[4] = {
		{"gradient", gen_gradient},
		{"photo", gen_photo},
		{"pixelart", gen_pixelart},
		{"noise", gen_noise}
	};
	unsigned char *orig;
	unsigned char *enc_l, *enc_nl;
	unsigned char *dec_std, *dec_near;
	qoi_desc desc, d1, d2;
	qoi_nl_desc nl;
	int len_l = 0, len_nl = 0;
	int m, i, max_abs;
	double mse, psnr;

	orig = (unsigned char *)malloc(NLEN);
	check(orig != NULL, "alloc sample buffer");

	printf("== Part A: correctness / contract checks ==\n");

	/* A1: tolerance 0 must be byte-for-byte identical to qoi_encode, for
	every material and every combination of the other knobs. */
	for (m = 0; m < 4; m++) {
		mats[m].gen(orig);
		desc = make_desc();
		enc_l = (unsigned char *)qoi_encode(orig, &desc, &len_l);
		for (i = 0; i < 4; i++) {
			nl.tolerance = 0;
			nl.dither = (unsigned char)(i & 1);
			nl.record_error = (unsigned char)((i >> 1) & 1);
			enc_nl = (unsigned char *)qoi_encode_near(orig, &desc, &nl, &len_nl);
			check(enc_nl != NULL && len_nl == len_l &&
				memcmp(enc_nl, enc_l, (size_t)len_l) == 0,
				"t=0 output byte-identical to qoi_encode");
			free(enc_nl);
		}
		free(enc_l);
	}

	/* A2: original lossless encode/decode round-trips exactly. */
	mats[1].gen(orig);
	desc = make_desc();
	enc_l = (unsigned char *)qoi_encode(orig, &desc, &len_l);
	dec_std = (unsigned char *)qoi_decode(enc_l, len_l, &d1, CHANNELS);
	check(dec_std != NULL &&
		memcmp(dec_std, orig, NLEN) == 0, "lossless qoi round-trip exact");
	free(dec_std);
	free(enc_l);

	/* A3: no-dither error bound |e| <= tolerance, swept over tolerances and
	materials, and standard qoi_decode reads the near-lossless stream. */
	for (m = 0; m < 4; m++) {
		mats[m].gen(orig);
		desc = make_desc();
		for (i = 0; i <= 30; i++) {
			int tol = i; /* 1..30 plus t=0 handled above */
			if (tol == 0) { continue; }
			nl.tolerance = (unsigned char)tol;
			nl.dither = 0;
			nl.record_error = 0;
			enc_nl = (unsigned char *)qoi_encode_near(orig, &desc, &nl, &len_nl);
			dec_std = (unsigned char *)qoi_decode(enc_nl, len_nl, &d1, CHANNELS);
			metrics(orig, dec_std, &max_abs, &mse, &psnr);
			check(max_abs <= tol, "no-dither max error within tolerance");
			free(dec_std);
			free(enc_nl);
		}
	}

	/* A4: record_error reconstructs the original exactly, with and without
	dither; the residual trailer is ignored by the standard decoder. */
	for (i = 0; i < 2; i++) {
		nl.tolerance = 7;
		nl.dither = (unsigned char)i;
		nl.record_error = 1;
		enc_nl = (unsigned char *)qoi_encode_near(orig, &desc, &nl, &len_nl);
		dec_near = (unsigned char *)qoi_decode_near(enc_nl, len_nl, &d1, CHANNELS);
		check(dec_near != NULL &&
			memcmp(dec_near, orig, NLEN) == 0,
			"record_error reconstructs original exactly");
		dec_std = (unsigned char *)qoi_decode(enc_nl, len_nl, &d2, CHANNELS);
		check(dec_std != NULL, "standard decoder tolerates residual trailer");
		free(dec_near);
		free(dec_std);
		free(enc_nl);
	}

	/* A5: a stream without a trailer decodes identically in both decoders. */
	nl.tolerance = 5; nl.dither = 0; nl.record_error = 0;
	enc_nl = (unsigned char *)qoi_encode_near(orig, &desc, &nl, &len_nl);
	dec_std = (unsigned char *)qoi_decode(enc_nl, len_nl, &d1, CHANNELS);
	dec_near = (unsigned char *)qoi_decode_near(enc_nl, len_nl, &d2, CHANNELS);
	check(memcmp(dec_std, dec_near, NLEN) == 0,
		"trailer-less stream: both decoders agree");
	free(dec_std); free(dec_near); free(enc_nl);

	/* A8: a corrupted residual trailer must not crash; the decoder falls
	back to the quantized image, which still satisfies the tolerance. */
	{
		unsigned char *mut;
		nl.tolerance = 6; nl.dither = 0; nl.record_error = 1;
		enc_nl = (unsigned char *)qoi_encode_near(orig, &desc, &nl, &len_nl);
		mut = (unsigned char *)malloc((size_t)len_nl);
		check(mut != NULL, "alloc corruption buffer");
		memcpy(mut, enc_nl, (size_t)len_nl);
		mut[len_nl - 1] ^= 0x5a; /* corrupt the checksum byte */
		dec_near = (unsigned char *)qoi_decode_near(mut, len_nl, &d1, CHANNELS);
		dec_std = (unsigned char *)qoi_decode(mut, len_nl, &d2, CHANNELS);
		check(dec_near != NULL && dec_std != NULL &&
			memcmp(dec_near, dec_std, NLEN) == 0,
			"corrupt trailer: graceful fallback to quantized image");
		metrics(orig, dec_near, &max_abs, &mse, &psnr);
		check(max_abs <= 6, "fallback image still within tolerance");
		free(dec_near); free(dec_std); free(mut); free(enc_nl);
	}

	/* A8b: a marker-like byte sequence appended AFTER the real residual
	trailer must be ignored. The real trailer (and its checksum) is left
	intact, so exact reconstruction still has to succeed - this exercises
	the candidate validation purely through the scan, without mutating any
	real residual bytes. */
	{
		unsigned char *appended;
		int app_len, k;
		nl.tolerance = 3; nl.dither = 0; nl.record_error = 1;
		enc_nl = (unsigned char *)qoi_encode_near(orig, &desc, &nl, &len_nl);
		app_len = len_nl + 12;
		appended = (unsigned char *)malloc((size_t)app_len);
		check(appended != NULL, "alloc appended-marker buffer");
		memcpy(appended, enc_nl, (size_t)len_nl);
		for (k = 0; k < 7; k++) { appended[len_nl + k] = 0; }
		appended[len_nl + 7] = 1;
		appended[len_nl + 8] = 'Q';
		appended[len_nl + 9] = 'N';
		appended[len_nl + 10] = 'L';
		appended[len_nl + 11] = 'R';
		dec_near =
			(unsigned char *)qoi_decode_near(appended, app_len, &d1, CHANNELS);
		check(dec_near != NULL &&
			memcmp(dec_near, orig, NLEN) == 0,
			"trailing fake marker ignored; real trailer reconstructs exactly");
		free(dec_near); free(appended); free(enc_nl);
	}

	/* A6: invalid parameter handling. */
	{
		int dummy = 0;
		nl.tolerance = 4; nl.dither = 0; nl.record_error = 0;
		check(qoi_encode_near(NULL, &desc, &nl, &dummy) == NULL,
			"NULL data rejected");
		nl.tolerance = 128;
		check(qoi_encode_near(orig, &desc, &nl, &dummy) == NULL,
			"tolerance > 127 rejected");
		check(qoi_encode_near(orig, &desc, NULL, &dummy) == NULL,
			"NULL options rejected");
	}

	/* A7: RGB (3-channel) path also quantizes and round-trips. */
	{
		const int w3 = 16, h3 = 16, len3 = w3 * h3 * 3;
		unsigned char *rgb = (unsigned char *)malloc(len3);
		unsigned char *e3, *d3;
		qoi_desc d3desc;
		qoi_nl_desc n3;
		int l3 = 0, k, ma;
		for (k = 0; k < len3; k++) { rgb[k] = (unsigned char)(rnd() & 0xff); }
		d3desc.width = w3; d3desc.height = h3;
		d3desc.channels = 3; d3desc.colorspace = QOI_SRGB;
		n3.tolerance = 6; n3.dither = 0; n3.record_error = 0;
		e3 = (unsigned char *)qoi_encode_near(rgb, &d3desc, &n3, &l3);
		d3 = (unsigned char *)qoi_decode_near(e3, l3, &d3desc, 3);
		check(e3 != NULL && d3 != NULL, "RGB near-lossless encode/decode");
		/* Bound check over the RGB buffer only (metrics() is RGBA-sized). */
		ma = 0;
		for (k = 0; k < len3; k++) {
			int dd = (int)rgb[k] - (int)d3[k];
			if (dd < 0) { dd = -dd; }
			if (dd > ma) { ma = dd; }
		}
		check(ma <= 6, "RGB no-dither error within tolerance");
		free(rgb); free(e3); free(d3);
	}

	printf("\n== Part B: four-material benchmark (64x64 RGBA, %d iters) ==\n",
		ITERS);
	printf("  reference size = lossless qoi_encode() output per material\n");
	for (m = 0; m < 4; m++) {
		mats[m].gen(orig);
		desc = make_desc();
		enc_l = (unsigned char *)qoi_encode(orig, &desc, &len_l);
		printf("  [%s] lossless qoi size = %d bytes\n", mats[m].name, len_l);
		bench_row(mats[m].name, orig, len_l, 4, 0, 0);
		bench_row(mats[m].name, orig, len_l, 4, 1, 0);
		bench_row(mats[m].name, orig, len_l, 4, 0, 1);
		free(enc_l);
	}

	printf("\n== Part C: tolerance sweep on photo (dither off, rec off) ==\n");
	mats[1].gen(orig);
	desc = make_desc();
	enc_l = (unsigned char *)qoi_encode(orig, &desc, &len_l);
	for (i = 0; i < 5; i++) {
		static const int tols[5] = {1, 2, 4, 8, 16};
		bench_row("photo", orig, len_l, tols[i], 0, 0);
	}
	free(enc_l);

	free(orig);

	printf("\n%d checks, %d failure(s)\n", g_checks, g_failures);
	if (g_failures == 0) {
		printf("ALL TESTS PASSED\n");
		return 0;
	}
	printf("TESTS FAILED\n");
	return 1;
}
