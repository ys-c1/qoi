/* Acceptance tests for the reentrant qoi encoder.
 *
 *   1. Byte-exact diff: 200 random images, qoi_encode() vs. the pristine
 *      reference implementation from git HEAD (and vs. qoi_encode_ws()).
 *   2. Concurrency: 8 threads encoding 100 images with qoi_encode_ws(),
 *      results must match the serial run bit for bit.
 *   3. Memory: RSS before/after 1000 consecutive encodes must not grow.
 *   4. Performance: per-image encode time vs. the reference, <= 5% regression.
 *
 * Build:  gcc -std=c99 -O2 -Wall -Wextra tests/qoi_mt_test.c tests/qoi_ref_impl.c -o tests/qoi_mt_test -lpthread
 * Run:    ./tests/qoi_mt_test
 */

#define _POSIX_C_SOURCE 199309L
#define QOI_IMPLEMENTATION
#include "../qoi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <time.h>

/* From tests/qoi_ref_impl.c (pristine HEAD implementation) */
void *qoi_ref_encode(const void *data, const qoi_desc *desc, int *out_len);

/* ------------------------------------------------------------------ */
/* Deterministic PRNG + synthetic images                               */

static uint64_t rng_state;
static uint64_t rng_next(void) {
	uint64_t x = rng_state;
	x ^= x << 13; x ^= x >> 7; x ^= x << 17;
	rng_state = x;
	return x;
}

typedef struct {
	qoi_desc desc;
	unsigned char *pixels;
} image_t;

/* pattern: 0 = noise, 1 = solid, 2 = gradient, 3 = run-heavy stripes */
static image_t gen_image(uint64_t seed, int pattern, int max_dim) {
	image_t img;
	uint64_t saved = rng_state;
	unsigned i, n;

	rng_state = seed;
	img.desc.width  = 1 + (unsigned)(rng_next() % max_dim);
	img.desc.height = 1 + (unsigned)(rng_next() % max_dim);
	img.desc.channels  = 3 + (unsigned char)(rng_next() & 1);
	img.desc.colorspace = (unsigned char)(rng_next() & 1);

	n = img.desc.width * img.desc.height * img.desc.channels;
	img.pixels = malloc(n);

	switch (pattern) {
	case 0: /* noise */
		for (i = 0; i < n; i++) img.pixels[i] = (unsigned char)rng_next();
		break;
	case 1: /* solid */
		memset(img.pixels, (int)(rng_next() & 0xff), n);
		break;
	case 2: /* smooth gradient */
		for (i = 0; i < n; i++)
			img.pixels[i] = (unsigned char)((i / img.desc.channels + i) & 0xff);
		break;
	default: /* run-heavy stripes */
		for (i = 0; i < n; i++)
			img.pixels[i] = (unsigned char)(((i / img.desc.channels) / 7) * 40);
		break;
	}

	rng_state = saved;
	return img;
}

static double now_sec(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static long rss_bytes(void) {
	FILE *f = fopen("/proc/self/statm", "r");
	long pages = 0, resident = 0;
	if (!f) return -1;
	if (fscanf(f, "%ld %ld", &pages, &resident) != 2) resident = -1;
	fclose(f);
	return resident * 4096;
}

/* ------------------------------------------------------------------ */
/* Test 1: byte-exact differential, 200 random images                  */

static int test_diff_200(void) {
	int fails = 0, i;
	for (i = 0; i < 200; i++) {
		image_t img = gen_image(0xC0FFEE + i * 7919, i % 4, 193);
		int ref_len = 0, new_len = 0, ws_len = 0;
		void *ref = qoi_ref_encode(img.pixels, &img.desc, &ref_len);
		void *new = qoi_encode(img.pixels, &img.desc, &new_len);

		qoi_encoder_workspace ws;
		int cap = qoi_encode_max_size(&img.desc);
		unsigned char *ws_out = malloc(cap);
		ws_len = qoi_encode_ws(img.pixels, &img.desc, &ws, ws_out, cap);

		if (!ref || !new || ref_len != new_len || memcmp(ref, new, ref_len) != 0) {
			printf("  FAIL diff img %d (qoi_encode): ref_len=%d new_len=%d\n", i, ref_len, new_len);
			fails++;
		}
		if (ws_len != ref_len || memcmp(ws_out, ref, ref_len) != 0) {
			printf("  FAIL diff img %d (qoi_encode_ws)\n", i);
			fails++;
		}
		free(ref); free(new); free(ws_out); free(img.pixels);
	}
	printf("test 1 (byte-exact diff, 200 images): %s\n", fails ? "FAIL" : "PASS");
	return fails;
}

/* ------------------------------------------------------------------ */
/* Test 2: 8 threads x 100 images, must match serial                   */

#define MT_IMAGES 100
#define MT_THREADS 8

typedef struct {
	image_t images[MT_IMAGES];
	unsigned char *serial[MT_IMAGES];
	int serial_len[MT_IMAGES];
	unsigned char *par[MT_IMAGES];
	int par_len[MT_IMAGES];
} mt_ctx_t;

typedef struct {
	mt_ctx_t *ctx;
	int tid;
} mt_arg_t;

static void *mt_run(void *arg_) {
	mt_arg_t *arg = arg_;
	qoi_encoder_workspace ws; /* one workspace per thread, on its stack */
	int i;
	for (i = arg->tid; i < MT_IMAGES; i += MT_THREADS) {
		image_t *img = &arg->ctx->images[i];
		int cap = qoi_encode_max_size(&img->desc);
		unsigned char *out = malloc(cap);
		arg->ctx->par_len[i] = qoi_encode_ws(img->pixels, &img->desc, &ws, out, cap);
		arg->ctx->par[i] = out;
	}
	return NULL;
}

static int test_concurrent(void) {
	static mt_ctx_t ctx;
	pthread_t th[MT_THREADS];
	mt_arg_t args[MT_THREADS];
	int i, fails = 0;

	for (i = 0; i < MT_IMAGES; i++) {
		ctx.images[i] = gen_image(0xBEEF + i * 104729, i % 4, 129);
		int cap = qoi_encode_max_size(&ctx.images[i].desc);
		qoi_encoder_workspace ws;
		ctx.serial[i] = malloc(cap);
		ctx.serial_len[i] = qoi_encode_ws(ctx.images[i].pixels, &ctx.images[i].desc,
		                                  &ws, ctx.serial[i], cap);
	}

	for (i = 0; i < MT_THREADS; i++) {
		args[i].ctx = &ctx; args[i].tid = i;
		pthread_create(&th[i], NULL, mt_run, &args[i]);
	}
	for (i = 0; i < MT_THREADS; i++) pthread_join(th[i], NULL);

	for (i = 0; i < MT_IMAGES; i++) {
		if (ctx.par_len[i] != ctx.serial_len[i] ||
		    memcmp(ctx.par[i], ctx.serial[i], ctx.serial_len[i]) != 0) {
			printf("  FAIL concurrency img %d\n", i);
			fails++;
		}
		free(ctx.serial[i]); free(ctx.par[i]); free(ctx.images[i].pixels);
	}
	printf("test 2 (8 threads x %d images == serial): %s\n", MT_IMAGES, fails ? "FAIL" : "PASS");
	return fails;
}

/* ------------------------------------------------------------------ */
/* Test 3: RSS stable over 1000 consecutive encodes                    */

static long rss_after_encodes(image_t *img, int use_ref, int iters) {
	long rss;
	int i, len = 0;
	for (i = 0; i < iters; i++) {
		void *enc = use_ref
			? qoi_ref_encode(img->pixels, &img->desc, &len)
			: qoi_encode(img->pixels, &img->desc, &len);
		if (!enc) return -1;
		free(enc);
	}
	rss = rss_bytes();
	return rss;
}

static int test_rss(void) {
	image_t img = gen_image(0xD15EA5E, 0, 512);
	long new_before, new_after, ref_before, ref_after;
	int fails = 0;

	/* generous warmup: lets glibc's dynamic mmap threshold and heap
	geometry settle, so we measure leaks rather than allocator growth */
	rss_after_encodes(&img, 0, 300);
	rss_after_encodes(&img, 1, 300);

	new_before = rss_bytes();
	new_after  = rss_after_encodes(&img, 0, 1000);
	ref_before = rss_bytes();
	ref_after  = rss_after_encodes(&img, 1, 1000);

	printf("test 3 (RSS over 1000 encodes):\n");
	printf("  new qoi_encode      : before=%ld KB after=%ld KB delta=%+ld KB\n",
	       new_before / 1024, new_after / 1024, (new_after - new_before) / 1024);
	printf("  reference (control) : before=%ld KB after=%ld KB delta=%+ld KB\n",
	       ref_before / 1024, ref_after / 1024, (ref_after - ref_before) / 1024);
	fails = (new_after < 0) || (new_after > new_before);
	printf("  no growth: %s\n", fails ? "FAIL" : "PASS");
	free(img.pixels);
	return fails;
}

/* ------------------------------------------------------------------ */
/* Test 4: performance vs. reference, <= 5% regression                 */

static int test_perf(void) {
	enum { REPS = 400, ROUNDS = 9 };
	image_t imgs[2];
	double best_ref = 1e30, best_new = 1e30, best_ws = 1e30;
	int k, r, len = 0;
	volatile unsigned char sink = 0;

	imgs[0] = gen_image(0xA, 0, 640); /* noise: worst case */
	imgs[1] = gen_image(0xB, 2, 640); /* gradient: typical case */

	for (r = 0; r < ROUNDS; r++) {
		double t0, t1, t2, t3;
		for (k = 0; k < 2; k++) {
			t0 = now_sec();
			for (int i = 0; i < REPS; i++) {
				void *e = qoi_ref_encode(imgs[k].pixels, &imgs[k].desc, &len);
				sink ^= ((unsigned char *)e)[0]; free(e);
			}
			t1 = now_sec();
			for (int i = 0; i < REPS; i++) {
				void *e = qoi_encode(imgs[k].pixels, &imgs[k].desc, &len);
				sink ^= ((unsigned char *)e)[0]; free(e);
			}
			t2 = now_sec();
			{
				qoi_encoder_workspace ws;
				int cap = qoi_encode_max_size(&imgs[k].desc);
				unsigned char *out = malloc(cap);
				for (int i = 0; i < REPS; i++)
					sink ^= (unsigned char)qoi_encode_ws(imgs[k].pixels, &imgs[k].desc, &ws, out, cap);
				free(out);
			}
			t3 = now_sec();
			if (t1 - t0 < best_ref) best_ref = t1 - t0;
			if (t2 - t1 < best_new) best_new = t2 - t1;
			if (t3 - t2 < best_ws)  best_ws  = t3 - t2;
		}
	}
	(void)sink;

	{
		double per_ref = best_ref / (2 * REPS) * 1e6;
		double per_new = best_new / (2 * REPS) * 1e6;
		double per_ws  = best_ws  / (2 * REPS) * 1e6;
		double regress = (best_new - best_ref) / best_ref * 100.0;
		printf("test 4 (perf, best of %d x %d encodes):\n", ROUNDS, 2 * REPS);
		printf("  reference qoi_encode : %8.2f us/image\n", per_ref);
		printf("  new       qoi_encode : %8.2f us/image  (%+.2f%%)\n", per_new, regress);
		printf("  new       qoi_encode_ws (no alloc): %8.2f us/image  (%+.2f%%)\n",
		       per_ws, (best_ws - best_ref) / best_ref * 100.0);
		printf("  regression <= 5%%: %s\n", regress <= 5.0 ? "PASS" : "FAIL");
		free(imgs[0].pixels); free(imgs[1].pixels);
		return regress > 5.0;
	}
}

int main(void) {
	int fails = 0;
	rng_state = 0x123456789abcdefULL;
	fails += test_diff_200();
	fails += test_concurrent();
	fails += test_rss();
	fails += test_perf();
	printf(fails ? "OVERALL: FAIL\n" : "OVERALL: PASS\n");
	return fails ? 1 : 0;
}
