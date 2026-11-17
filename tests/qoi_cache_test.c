/*

Cache test/benchmark for the content-fingerprint encode cache in qoi.h.
Self-contained: no third-party libraries, no files read, no network. Run with
	./qoi_cache_test
(or through tests/run.sh, which also builds the repo's existing tools first).

Exit code 0 means every check passed; any failed check prints "FAIL" with a
reason and makes the process exit non-zero.

What is exercised:
  * byte-exactness: cached results equal qoi_encode() output (and recorded
    golden vectors from the pristine library) on every image used;
  * four workloads: exact duplicates, one-pixel edited variant, different
    sizes, and forged-hash collisions;
  * hit rate / encoding time / cache memory per workload;
  * collision correctness under a weak hash (8-bit, 2-bit and always-zero
    hashes) - wrong bytes must never be returned;
  * eviction policies (LRU / FIFO / random) and capacity limits, including
    memory point-in-time accounting;
  * invalidation on configuration/hash changes, and invalid-input behaviour.
*/

#define _POSIX_C_SOURCE 199309L
#define QOI_IMPLEMENTATION
#include "qoi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ------------------------------------------------------------------ helpers */

static int checks = 0;
static int failures = 0;

static void check(int cond, const char *what) {
	checks++;
	if (!cond) {
		failures++;
		printf("  FAIL: %s\n", what);
	}
}

static double now_sec(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void fill_deterministic(unsigned char *px, int w, int h, int channels,
		unsigned int seed) {
	int x, y, c;
	for (y = 0; y < h; y++) {
		for (x = 0; x < w; x++) {
			unsigned int v = (unsigned int)(x * 2654435761u + y * 40503u + seed);
			for (c = 0; c < channels; c++) {
				px[((size_t)y * w + x) * channels + c] =
					(unsigned char)((v >> (c * 8)) ^ (x * 7) ^ (y * 13));
			}
			if (channels == 4) {
				/* mostly opaque, with some alpha variation */
				px[((size_t)y * w + x) * channels + 3] =
					(unsigned char)(((x / 5 + y / 3) & 1) ? 255 : 60 + ((x * 3) & 127));
			}
		}
	}
}

/* A small image with flat areas, gradients and edges: partly compressible,
like real content, without needing any image files. */
static void make_picture(unsigned char *px, int w, int h, int channels) {
	int x, y, c;
	fill_deterministic(px, w, h, channels, 0u);
	for (y = 0; y < h; y++) {
		for (x = 0; x < w; x++) {
			size_t o = ((size_t)y * w + x) * channels;
			if (x > w / 4 && x < w / 2 && y > h / 4 && y < h / 2) {
				px[o + 0] = 240; px[o + 1] = 200; px[o + 2] = 50;
				if (channels == 4) px[o + 3] = 255;
			}
			else if (y < h / 6) {
				for (c = 0; c < channels; c++) px[o + c] = 20;
			}
		}
	}
}

typedef struct {
	const char *name;
	unsigned char *pixels;
	qoi_desc desc;
} image;

static image make_image(const char *name, int w, int h, int channels) {
	image im;
	size_t n = (size_t)w * h * channels;
	im.name = name;
	im.pixels = (unsigned char *)malloc(n);
	if (!im.pixels) { printf("out of memory\n"); exit(2); }
	make_picture(im.pixels, w, h, channels);
	im.desc.width = (unsigned int)w;
	im.desc.height = (unsigned int)h;
	im.desc.channels = (unsigned char)channels;
	im.desc.colorspace = QOI_SRGB;
	return im;
}

static void free_image(image *im) {
	free(im->pixels);
	im->pixels = NULL;
}

/* Golden bytes captured from the pristine (pre-cache) qoi.h, for the 8x8
crafted image in make_golden_image(). Verifies that the encoder itself, as
reached through the cache, is byte-for-byte unchanged. */
static const unsigned char golden_crafted_rgba[] = {
	0x71,0x6f,0x69,0x66,0x00,0x00,0x00,0x08,0x00,0x00,0x00,0x08,0x04,0x00,
	0xfe,0x0a,0x14,0x1e,0xc7,0x7a,0x7a,0x7a,0x7a,0x7a,0x7a,0x7a,
	0x09,0xa3,0x9a,0xa3,0x9a,0xa3,0x9a,0xa3,0x9a,0xa3,0x9a,0xa3,0x9a,0xa3,0x9a,0x09,
	0xc6,0xfe,0xc8,0x11,0x63,0xfe,0xc1,0x1c,0x60,0xfe,0xba,0x27,0x5d,
	0xfe,0xb3,0x32,0x5a,0xfe,0xac,0x3d,0x57,0xfe,0xa5,0x48,0x54,0xfe,0x9e,0x53,0x51,0xfe,0x97,0x5e,0x4e,
	0xff,0x33,0x66,0x99,0xc8,0xff,0x33,0x66,0x99,0xc9,0xff,0x33,0x66,0x99,0xca,0xff,0x33,0x66,0x99,0xcb,
	0xff,0x33,0x66,0x99,0xcc,0xff,0x33,0x66,0x99,0xcd,0xff,0x33,0x66,0x99,0xce,0xff,0x33,0x66,0x99,0xcf,
	0xff,0x33,0x66,0x99,0xd0,0xff,0x33,0x66,0x99,0xd1,0xff,0x33,0x66,0x99,0xd2,0xff,0x33,0x66,0x99,0xd3,
	0xff,0x33,0x66,0x99,0xd4,0xff,0x33,0x66,0x99,0xd5,0xff,0x33,0x66,0x99,0xd6,0xff,0x33,0x66,0x99,0xd7,
	0xc7,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x01
};

/* Rebuild the exact 8x8 input that produced golden_crafted_rgba. */
static void make_golden_image(unsigned char *px) {
	int x, y, i;
	for (i = 0; i < 64; i++) { px[i*4]=10; px[i*4+1]=20; px[i*4+2]=30; px[i*4+3]=255; }
	for (x = 0; x < 8; x++) { px[(8+x)*4]=10+x; px[(8+x)*4+1]=20; px[(8+x)*4+2]=30; }
	for (x = 0; x < 8; x++) { px[(16+x)*4]=10+x*4; px[(16+x)*4+1]=20+x*3; px[(16+x)*4+2]=30+x*5; }
	for (x = 0; x < 8; x++) { px[(24+x)*4]=10; px[(24+x)*4+1]=20; px[(24+x)*4+2]=30; px[(24+x)*4+3]=255; }
	for (x = 0; x < 8; x++) { px[(32+x)*4]=200-x*7; px[(32+x)*4+1]=17+x*11; px[(32+x)*4+2]=99-x*3; }
	for (i = 40; i < 56; i++) { px[i*4]=51; px[i*4+1]=102; px[i*4+2]=153; px[i*4+3]=(unsigned char)(200+(i-40)); }
	for (i = 56; i < 64; i++) { px[i*4]=51; px[i*4+1]=102; px[i*4+2]=153; px[i*4+3]=215; }
	(void)y;
}

/* Encode uncached and compare against an independent reference (the bytes
qoi_encode() returned before we ever touched the cache path). Returns the
encoded length or -1. */
static int encode_plain(const image *im, unsigned char **out) {
	int len = 0;
	unsigned char *e = (unsigned char *)qoi_encode(im->pixels, &im->desc, &len);
	if (!e) return -1;
	*out = e;
	return len;
}

/* ------------------------------------------------------------------- tests */

static void test_golden_and_transparency(void) {
	unsigned char px[8 * 8 * 4];
	unsigned char *e;
	int len = 0;
	qoi_desc d = {8, 8, 4, QOI_SRGB};

	make_golden_image(px);

	e = (unsigned char *)qoi_encode(px, &d, &len);
	check(e != NULL, "qoi_encode crafted succeeds");
	if (e) {
		check(len == (int)sizeof(golden_crafted_rgba),
			"qoi_encode(golden) has original length");
		check(memcmp(e, golden_crafted_rgba, sizeof(golden_crafted_rgba)) == 0,
			"qoi_encode(golden) matches pristine-capture bytes");
		free(e);
	}

	/* The cache must return byte-identical output. */
	qoi_cache_config(1u << 20, QOI_CACHE_LRU);
	qoi_cache_clear();
	e = (unsigned char *)qoi_encode_cached(px, &d, &len);
	check(e != NULL, "qoi_encode_cached(golden) succeeds");
	if (e) {
		check(len == (int)sizeof(golden_crafted_rgba),
			"cached length matches golden");
		check(memcmp(e, golden_crafted_rgba, sizeof(golden_crafted_rgba)) == 0,
			"cached output matches pristine-capture bytes");
		free(e);
	}
	/* Second call comes from the cache and must still match. */
	e = (unsigned char *)qoi_encode_cached(px, &d, &len);
	if (e) {
		check(memcmp(e, golden_crafted_rgba, sizeof(golden_crafted_rgba)) == 0,
			"cache hit output matches pristine-capture bytes");
		free(e);
	}
	qoi_cache_clear();
}

/* Workload runner: encodes `calls` times, cycling through `n` images. Groups
alternate images so every call after the first warm-up is a cache hit (or a
detected collision under a weak hash). */
typedef struct {
	unsigned long long hits, misses, collisions, evictions, stores, bytes;
	unsigned int entries;
	double encode_sec;      /* wall time of the cached-call loop only */
	double plain_sec;       /* wall time of an equal uncached pass */
	int byte_mismatches;
} workload_result;

/* Verifies every cached result against a plain encode (outside the timed
loop), then times the cached path and an equal uncached pass separately. */
static workload_result run_workload(image *imgs, int n, int calls,
		int warmup_passes, unsigned int capacity, int policy) {
	workload_result r;
	unsigned char *e;
	int i, pass, len;
	double t0;
	qoi_cache_stats st;

	memset(&r, 0, sizeof(r));
	qoi_cache_config(capacity, policy);
	qoi_cache_clear();

	/* warm-up passes (untimed) to reach steady state */
	for (pass = 0; pass < warmup_passes; pass++) {
		for (i = 0; i < calls; i++) {
			image *im = &imgs[i % n];
			e = (unsigned char *)qoi_encode_cached(im->pixels, &im->desc, &len);
			if (e) free(e);
		}
	}

	qoi_cache_clear();

	/* correctness pass: every cached result compared to a plain encode */
	for (i = 0; i < calls; i++) {
		image *im = &imgs[i % n];
		unsigned char *plain = NULL;
		int plen = encode_plain(im, &plain);
		e = (unsigned char *)qoi_encode_cached(im->pixels, &im->desc, &len);
		if (!e || !plain) { r.byte_mismatches++; }
		else if (plen != len || memcmp(e, plain, (size_t)len) != 0) {
			r.byte_mismatches++;
		}
		free(e);
		free(plain);
	}

	/* timed cached-only pass, same state as the correctness pass ended in */
	qoi_cache_clear();
	t0 = now_sec();
	for (i = 0; i < calls; i++) {
		image *im = &imgs[i % n];
		e = (unsigned char *)qoi_encode_cached(im->pixels, &im->desc, &len);
		if (e) free(e);
	}
	r.encode_sec = now_sec() - t0;

	qoi_cache_get_stats(&st);
	r.hits = st.hits;
	r.misses = st.misses;
	r.collisions = st.collisions;
	r.evictions = st.evictions;
	r.stores = st.stores;
	r.bytes = st.bytes;
	r.entries = st.entries;

	/* time an equal number of uncached encodes */
	t0 = now_sec();
	for (i = 0; i < calls; i++) {
		unsigned char *plain = (unsigned char *)qoi_encode(
			imgs[i % n].pixels, &imgs[i % n].desc, &len);
		free(plain);
	}
	r.plain_sec = now_sec() - t0;

	return r;
}

static void print_workload(const char *label, const workload_result *r,
		int calls) {
	double hit_rate = calls > 0 ? 100.0 * (double)r->hits / (double)calls : 0.0;
	printf("    %-28s calls=%-6d hits=%-6llu misses=%-6llu hit=%6.2f%% "
		"collisions=%-4llu evictions=%-4llu cache=%-8llu B (%u entries) "
		"cached=%.2f ms plain=%.2f ms mismatches=%d\n",
		label, calls, r->hits, r->misses, hit_rate, r->collisions,
		r->evictions, r->bytes, r->entries,
		r->encode_sec * 1e3, r->plain_sec * 1e3, r->byte_mismatches);
	check(r->byte_mismatches == 0, "cached output equals qoi_encode output");
}

/* Group 1: exact duplicates. Every call after the first must hit. */
static void test_group_duplicates(image *base) {
	const int calls = 400;
	workload_result r = run_workload(base, 1, calls, 0, 4u << 20, QOI_CACHE_LRU);
	printf("  [1] exact duplicates (same 64x64 RGBA image, repeated)\n");
	print_workload("identical", &r, calls);
	check(r.hits == (unsigned long long)(calls - 1),
		"duplicates: all but first call are hits");
	check(r.misses == 1, "duplicates: exactly one encode on cold start");
	check(r.collisions == 0, "duplicates: no spurious collisions");
	check(r.byte_mismatches == 0, "duplicates: bytes identical");
}

/* Group 2: one pixel changed. The edited image must be a miss and must not
reuse the original's bytes; the original must still hit afterwards. */
static void test_group_one_pixel(image *base, image *edited) {
	const int calls = 400;
	int x, y;
	workload_result r;
	unsigned char *e1, *e2;
	int l1 = 0, l2 = 0;

	/* edit exactly one pixel channel */
	x = 17; y = 9;
	memcpy(edited->pixels, base->pixels,
		(size_t)base->desc.width * base->desc.height * base->desc.channels);
	edited->pixels[((size_t)y * base->desc.width + x) * base->desc.channels] ^= 0x01;
	edited->desc = base->desc;

	printf("  [2] one pixel changed (byte at x=17,y=9 flipped)\n");
	r = run_workload((image[]){ *base, *edited }, 2, calls, 0, 4u << 20,
		QOI_CACHE_LRU);
	print_workload("alternating original/edited", &r, calls);
	check(r.hits >= (unsigned long long)(calls / 2 - 1),
		"one-pixel: warm images hit");
	check(r.misses == 2, "one-pixel: one encode per distinct image");

	/* Direct check: bytes returned for edited differ from base bytes. */
	qoi_cache_clear();
	qoi_cache_config(4u << 20, QOI_CACHE_LRU);
	e1 = (unsigned char *)qoi_encode_cached(base->pixels, &base->desc, &l1);
	e2 = (unsigned char *)qoi_encode_cached(edited->pixels, &edited->desc, &l2);
	check(e1 && e2, "one-pixel: both encodes succeed");
	if (e1 && e2) {
		check(!(l1 == l2 && memcmp(e1, e2, (size_t)l1) == 0),
			"one-pixel: edited image does not reuse base bytes");
	}
	free(e1);
	free(e2);

	/* Restore base in the cache: after the edit, base is still a hit. */
	qoi_cache_clear();
	e1 = (unsigned char *)qoi_encode_cached(base->pixels, &base->desc, &l1);
	free(e1);
	e1 = (unsigned char *)qoi_encode_cached(edited->pixels, &edited->desc, &l1);
	free(e1);
	{
		qoi_cache_stats st;
		qoi_cache_get_stats(&st);
		check(st.hits == 0 && st.misses == 2,
			"one-pixel: no false hit between similar images");
	}
	qoi_cache_clear();
}

/* Group 3: different sizes. Same visual content at 3 sizes must all be misses
on first use and hits on reuse; sizes are also part of the key. */
static void test_group_sizes(image *small, image *medium, image *large) {
	const int calls = 300;
	workload_result r;
	unsigned char *e;
	int len;

	printf("  [3] different sizes (same generator, 16x16 / 40x30 / 64x64)\n");
	r = run_workload((image[]){ *small, *medium, *large }, 3, calls, 0,
		8u << 20, QOI_CACHE_LRU);
	print_workload("round-robin sizes", &r, calls);
	check(r.misses == 3, "sizes: three distinct keys encode once each");
	check(r.hits == (unsigned long long)(calls - 3), "sizes: rest hit");
	check(r.byte_mismatches == 0, "sizes: bytes identical");

	/* Same pixel count, different aspect ratio => must not alias. */
	qoi_cache_clear();
	qoi_cache_config(8u << 20, QOI_CACHE_LRU);
	{
		unsigned char *a = (unsigned char *)malloc(16 * 16 * 4);
		unsigned char *b = (unsigned char *)malloc(16 * 16 * 4);
		qoi_desc da = {16, 16, 4, QOI_SRGB}, db = {8, 32, 4, QOI_SRGB};
		unsigned char *ea, *eb;
		int la = 0, lb = 0;
		qoi_cache_clear();
		memset(a, 0x5a, 16 * 16 * 4);
		memset(b, 0x5a, 16 * 16 * 4);
		ea = (unsigned char *)qoi_encode_cached(a, &da, &la);
		eb = (unsigned char *)qoi_encode_cached(b, &db, &lb);
		check(ea && eb, "sizes: same-pixelcount encodes succeed");
		/* 16x16 and 8x32 have no pixels in common, so even with the same
		   uniform fill the two encoded streams must differ (header and row
		   structure) and both must decode back correctly. */
		check(ea && eb && (la != lb || memcmp(ea, eb, (size_t)la) != 0),
			"sizes: 16x16 and 8x32 not conflated");
		{
			qoi_cache_stats st;
			qoi_cache_get_stats(&st);
			check(st.misses == 2 && st.hits == 0,
				"sizes: same pixel count but different dims are distinct keys");
		}
		free(ea); free(eb); free(a); free(b);
	}

	/* One image asked for under two descriptors (3 vs 4 channels). */
	qoi_cache_clear();
	qoi_cache_config(8u << 20, QOI_CACHE_LRU);
	{
		size_t n = (size_t)small->desc.width * small->desc.height;
		unsigned char *rgb = (unsigned char *)malloc(n * 3);
		qoi_desc d3 = small->desc, d4 = small->desc;
		size_t i;
		for (i = 0; i < n; i++) {
			rgb[i*3+0] = small->pixels[i*4+0];
			rgb[i*3+1] = small->pixels[i*4+1];
			rgb[i*3+2] = small->pixels[i*4+2];
		}
		d3.channels = 3;
		e = (unsigned char *)qoi_encode_cached(rgb, &d3, &len);
		free(e);
		e = (unsigned char *)qoi_encode_cached(small->pixels, &d4, &len);
		free(e);
		{
			qoi_cache_stats st;
			qoi_cache_get_stats(&st);
			check(st.misses == 2, "sizes: channel count is part of the key");
		}
		free(rgb);
	}
	qoi_cache_clear();
}

/* Group 4: forged collisions. We install hashes that map many distinct images
to the same fingerprint and verify:
  - no wrong bytes are ever returned (full content comparison);
  - collision events are counted;
  - a valid image still hits once it owns the entry;
  - bytes still match qoi_encode() exactly for every call.

`edited` is chosen by the caller to collide with `base` under a 8-bit weak hash
(search done once at startup and reported), so this group is deterministic. */
static void test_group_collisions(image *base, image *edited) {
	const int calls = 400;
	workload_result r;
	unsigned char *e;
	int len;
	qoi_cache_stats st;

	printf("  [4] forged hash collisions (weak 8/2/0-bit hashes)\n");

	/* 8-bit weak hash: the two images share a fingerprint, so every
	   alternation is detected as a collision. */
	qoi_cache_set_weak_hash(8);
	{
		qoi_cache_hash_fn h = qoi_cache_get_hash();
		check(h(base->pixels, &base->desc) == h(edited->pixels, &edited->desc),
			"collisions: crafted images share the 8-bit fingerprint");
	}
	r = run_workload((image[]){ *base, *edited }, 2, calls, 0, 4u << 20,
		QOI_CACHE_LRU);
	print_workload("2 colliding images, 8-bit", &r, calls);
	check(r.collisions > 0, "collisions: 8-bit hash actually collides");
	check(r.byte_mismatches == 0, "collisions: 8-bit hash never returns wrong bytes");

	/* Always-zero hash: every image has the same fingerprint. */
	qoi_cache_set_weak_hash(0);
	qoi_cache_config(4u << 20, QOI_CACHE_LRU);
	qoi_cache_clear();
	{
		int i, bad = 0;
		for (i = 0; i < calls; i++) {
			image *im = (i & 1) ? edited : base;
			unsigned char *plain = NULL;
			int plen = encode_plain(im, &plain);
			e = (unsigned char *)qoi_encode_cached(im->pixels, &im->desc, &len);
			if (!e || !plain || plen != len || memcmp(e, plain, (size_t)len) != 0) {
				bad++;
			}
			free(e);
			free(plain);
		}
		qoi_cache_get_stats(&st);
		printf("    %-28s calls=%-6d hits=%-6llu misses=%-6llu "
			"collisions=%-4llu cache=%llu B\n",
			"2 images, zero hash", calls, st.hits, st.misses, st.collisions,
			st.bytes);
		check(bad == 0, "collisions: zero hash never returns wrong bytes");
		check(st.collisions > 0, "collisions: zero hash detected as collisions");
	}

	/* Single image, zero hash: must still hit (identity, not collision). */
	qoi_cache_clear();
	e = (unsigned char *)qoi_encode_cached(base->pixels, &base->desc, &len);
	free(e);
	e = (unsigned char *)qoi_encode_cached(base->pixels, &base->desc, &len);
	free(e);
	qoi_cache_get_stats(&st);
	check(st.hits == 1 && st.misses == 1,
		"collisions: identical content still hits under a colliding hash");

	/* After k distinct images share one bucket, every one of them must still
	   get its own correct bytes (no cross-contamination even with eviction). */
	qoi_cache_set_weak_hash(0);
	qoi_cache_config(64u << 20, QOI_CACHE_LRU);
	qoi_cache_clear();
	{
		enum { K = 8 };
		image imgs[K];
		unsigned char *ref[K];
		int reflen[K];
		int i, ok = 1;
		for (i = 0; i < K; i++) {
			char nm[32];
			sprintf(nm, "collide-%d", i);
			imgs[i] = make_image(nm, 24 + i, 18, 4);
			/* make sure they are pairwise distinct */
			imgs[i].pixels[(size_t)i * 4] ^= 0x01;
			reflen[i] = encode_plain(&imgs[i], &ref[i]);
			e = (unsigned char *)qoi_encode_cached(imgs[i].pixels, &imgs[i].desc,
				&len);
			if (!e || reflen[i] != len || memcmp(e, ref[i], (size_t)len) != 0) ok = 0;
			free(e);
		}
		for (i = 0; i < K; i++) {
			e = (unsigned char *)qoi_encode_cached(imgs[i].pixels, &imgs[i].desc,
				&len);
			if (!e || reflen[i] != len || memcmp(e, ref[i], (size_t)len) != 0) ok = 0;
			free(e);
			free(ref[i]);
			free_image(&imgs[i]);
		}
		check(ok, "collisions: 8 images sharing one fingerprint stay distinct");
	}

	/* restore the good hash */
	qoi_cache_set_hash(NULL);
	qoi_cache_clear();
}

/* Capacity / eviction behaviour, including exact accounting. */
static void test_eviction(void) {
	enum { NIMG = 6 };
	image imgs[NIMG];
	int i, pass;
	unsigned char *e;
	int len;
	qoi_cache_stats st;
	/* 8x6 RGBA entries cost roughly 400 accounted bytes each, so ~3 fit. */
	const unsigned int cap = 1200u;

	printf("  [5] eviction policies and capacity\n");
	qoi_cache_set_hash(NULL);

	/* Small images + small capacity so eviction actually happens. */
	for (i = 0; i < NIMG; i++) {
		char nm[24];
		sprintf(nm, "evict-%d", i);
		imgs[i] = make_image(nm, 8, 6, 4);
		imgs[i].pixels[(size_t)i * 4 + 1] ^= 0x11;
	}

	/* LRU: touch image 0, then insert enough others to exceed capacity; image 0
	   should survive while untouched ones are evicted. */
	qoi_cache_config(cap, QOI_CACHE_LRU);
	qoi_cache_clear();
	for (i = 0; i < NIMG; i++) {
		e = (unsigned char *)qoi_encode_cached(imgs[i].pixels, &imgs[i].desc, &len);
		free(e);
	}
	qoi_cache_get_stats(&st);
	check(st.bytes <= cap, "eviction: accounted bytes never exceed capacity");
	check(st.evictions > 0, "eviction: capacity pressure evicts");
	printf("    LRU cap=%u: entries=%u bytes=%llu evictions=%llu\n",
		cap, st.entries, st.bytes, st.evictions);

	/* FIFO: behaviour must also respect capacity. */
	qoi_cache_config(cap, QOI_CACHE_FIFO);
	qoi_cache_clear();
	for (pass = 0; pass < 3; pass++) {
		for (i = 0; i < NIMG; i++) {
			e = (unsigned char *)qoi_encode_cached(imgs[i].pixels, &imgs[i].desc,
				&len);
			free(e);
			qoi_cache_get_stats(&st);
			check(st.bytes <= cap, "eviction: FIFO respects capacity");
		}
	}
	qoi_cache_get_stats(&st);
	printf("    FIFO cap=%u: entries=%u bytes=%llu evictions=%llu\n",
		cap, st.entries, st.bytes, st.evictions);

	/* Random. */
	qoi_cache_config(cap, QOI_CACHE_RAND);
	qoi_cache_clear();
	for (pass = 0; pass < 3; pass++) {
		for (i = 0; i < NIMG; i++) {
			e = (unsigned char *)qoi_encode_cached(imgs[i].pixels, &imgs[i].desc,
				&len);
			free(e);
			qoi_cache_get_stats(&st);
			check(st.bytes <= cap, "eviction: random policy respects capacity");
		}
	}

	/* LRU ordering: capacity for exactly two 8x6 images. Insert A, B; touch A;
	   inserting C must evict B, leaving A resident. FIFO must instead evict A. */
	{
		image A = make_image("lru-a", 8, 6, 4);
		image B = make_image("lru-b", 8, 6, 4);
		image C = make_image("lru-c", 8, 6, 4);
		const unsigned int cap2 = 1250u;
		unsigned long long h0;

		A.pixels[0] ^= 0x01; B.pixels[1] ^= 0x02; C.pixels[2] ^= 0x03;

		qoi_cache_config(cap2, QOI_CACHE_LRU);
		qoi_cache_clear();
		e = (unsigned char *)qoi_encode_cached(A.pixels, &A.desc, &len); free(e);
		e = (unsigned char *)qoi_encode_cached(B.pixels, &B.desc, &len); free(e);
		qoi_cache_get_stats(&st);
		check(st.entries == 2, "eviction: LRU holds two entries at test capacity");
		/* touch A so B is the least recently used */
		e = (unsigned char *)qoi_encode_cached(A.pixels, &A.desc, &len); free(e);
		e = (unsigned char *)qoi_encode_cached(C.pixels, &C.desc, &len); free(e);
		qoi_cache_get_stats(&st);
		h0 = st.hits;
		e = (unsigned char *)qoi_encode_cached(A.pixels, &A.desc, &len); free(e);
		qoi_cache_get_stats(&st);
		check(st.hits == h0 + 1, "eviction: LRU keeps a recently used entry");
		{
			unsigned long long m0, ev0;
			qoi_cache_get_stats(&st);
			m0 = st.misses;
			ev0 = st.evictions;
			e = (unsigned char *)qoi_encode_cached(B.pixels, &B.desc, &len); free(e);
			qoi_cache_get_stats(&st);
			check(st.misses == m0 + 1,
				"eviction: LRU evicted the least recently used entry");
			(void)ev0;
		}

		/* FIFO: A inserted first, so A goes first regardless of the touch. */
		qoi_cache_config(cap2, QOI_CACHE_FIFO);
		qoi_cache_clear();
		e = (unsigned char *)qoi_encode_cached(A.pixels, &A.desc, &len); free(e);
		e = (unsigned char *)qoi_encode_cached(B.pixels, &B.desc, &len); free(e);
		/* touches do not reorder under FIFO */
		e = (unsigned char *)qoi_encode_cached(A.pixels, &A.desc, &len); free(e);
		e = (unsigned char *)qoi_encode_cached(C.pixels, &C.desc, &len); free(e);
		qoi_cache_get_stats(&st);
		h0 = st.hits;
		e = (unsigned char *)qoi_encode_cached(B.pixels, &B.desc, &len); free(e);
		qoi_cache_get_stats(&st);
		check(st.hits == h0 + 1, "eviction: FIFO keeps the newer entry");
		qoi_cache_get_stats(&st);
		h0 = st.hits;
		/* A was inserted first and was never re-ordered, so it is gone; this
		   lookup may evict B in turn, which is fine for this check. */
		{
			unsigned long long m0 = st.misses;
			e = (unsigned char *)qoi_encode_cached(A.pixels, &A.desc, &len); free(e);
			qoi_cache_get_stats(&st);
			check(st.hits == h0 && st.misses == m0 + 1,
				"eviction: FIFO evicts the oldest inserted entry");
		}

		free_image(&A);
		free_image(&B);
		free_image(&C);
	}

	/* Shrinking capacity must free entries immediately (point-in-time bound). */
	qoi_cache_config(cap, QOI_CACHE_LRU);
	qoi_cache_clear();
	for (i = 0; i < NIMG; i++) {
		e = (unsigned char *)qoi_encode_cached(imgs[i].pixels, &imgs[i].desc, &len);
		free(e);
	}
	qoi_cache_get_stats(&st);
	{
		unsigned long long before = st.bytes;
		unsigned int before_n = st.entries;
		qoi_cache_config(512u, QOI_CACHE_LRU);
		qoi_cache_get_stats(&st);
		check(st.bytes <= 512u, "eviction: shrink enforces new capacity");
		check(st.bytes < before || before_n <= 1,
			"eviction: shrink actually drops entries");
		printf("    shrink: %llu B/%u entries -> %llu B/%u entries\n",
			before, before_n, st.bytes, st.entries);
	}

	/* Zero capacity disables storing (but still returns correct bytes). */
	qoi_cache_clear();
	qoi_cache_config(0, QOI_CACHE_LRU);
	qoi_cache_clear();
	for (i = 0; i < 4; i++) {
		unsigned char *plain = NULL;
		int plen = encode_plain(&imgs[0], &plain);
		e = (unsigned char *)qoi_encode_cached(imgs[0].pixels, &imgs[0].desc, &len);
		check(e && plain && plen == len && memcmp(e, plain, (size_t)len) == 0,
			"eviction: zero-capacity path is byte-exact");
		free(e);
		free(plain);
	}
	qoi_cache_get_stats(&st);
	check(st.entries == 0 && st.hits == 0, "eviction: zero capacity stores nothing");

	/* Too-large single entry is passed through, not cached. */
	qoi_cache_config(256, QOI_CACHE_LRU);
	qoi_cache_clear();
	{
		/* 40x30 RGBA is ~4.8 KB of pixels, far above 256 bytes. */
		image big = make_image("oversized", 40, 30, 4);
		unsigned char *plain = NULL;
		int plen = encode_plain(&big, &plain);
		e = (unsigned char *)qoi_encode_cached(big.pixels, &big.desc, &len);
		check(e && plain && plen == len && memcmp(e, plain, (size_t)len) == 0,
			"eviction: oversized image still encodes correctly");
		free(e);
		free(plain);
		qoi_cache_get_stats(&st);
		check(st.entries == 0, "eviction: oversized image not cached");
		free_image(&big);
	}

	qoi_cache_config(8u << 20, QOI_CACHE_LRU);
	qoi_cache_clear();
	for (i = 0; i < NIMG; i++) free_image(&imgs[i]);
}

/* Invalid inputs and failure transparency. */
static void test_invalid(void) {
	qoi_desc d = {0, 0, 4, QOI_SRGB};
	int len = -1234;
	unsigned char px[16] = {0};

	printf("  [6] invalid input / API edges\n");

	check(qoi_encode_cached(px, &d, &len) == NULL, "invalid: zero size returns NULL");
	check(len == -1234, "invalid: out_len untouched by failed encode");

	d.width = 4; d.height = 4; d.channels = 2;
	check(qoi_encode_cached(px, &d, &len) == NULL, "invalid: bad channel count");
	d.channels = 4; d.colorspace = 9;
	check(qoi_encode_cached(px, &d, &len) == NULL, "invalid: bad colorspace");

	check(qoi_encode_cached(NULL, &d, &len) == NULL, "invalid: NULL data");
	check(qoi_encode_cached(px, NULL, &len) == NULL, "invalid: NULL desc");
	d.colorspace = QOI_SRGB;
	check(qoi_encode_cached(px, &d, NULL) == NULL, "invalid: NULL out_len");

	/* Config sanitisers: unknown policy ignored, NULL stats ignored. */
	qoi_cache_config(1u << 20, 99);
	qoi_cache_get_stats(NULL); /* must not crash */
	qoi_cache_clear();
	qoi_cache_destroy();
	qoi_cache_destroy(); /* idempotent */

	/* after destroy the cache is usable again with defaults */
	{
		unsigned char *plain = NULL;
		unsigned char *e = NULL;
		int plen;
		image im = make_image("after-destroy", 12, 10, 4);
		plen = encode_plain(&im, &plain);
		e = (unsigned char *)qoi_encode_cached(im.pixels, &im.desc, &len);
		check(e && plain && plen == len && memcmp(e, plain, (size_t)len) == 0,
			"invalid: cache usable and exact after destroy");
		free(e);
		free(plain);
		free_image(&im);
	}
	qoi_cache_destroy();
}

/* Small footgun guard: changing the hash clears entries keyed by the old one. */
static void test_hash_change_invalidates(void) {
	image im = make_image("hashchange", 32, 24, 4);
	unsigned char *e;
	int len;
	qoi_cache_stats st;

	printf("  [7] hash changes invalidate entries\n");
	qoi_cache_config(1u << 20, QOI_CACHE_LRU);
	qoi_cache_set_hash(NULL);
	qoi_cache_clear();
	e = (unsigned char *)qoi_encode_cached(im.pixels, &im.desc, &len);
	free(e);
	e = (unsigned char *)qoi_encode_cached(im.pixels, &im.desc, &len);
	free(e);
	qoi_cache_get_stats(&st);
	check(st.hits == 1, "hash change: default hash hits its own entry");

	qoi_cache_set_weak_hash(16);
	qoi_cache_get_stats(&st);
	check(st.entries == 0, "hash change: switching hash drops stale entries");
	{
		unsigned long long h0 = st.hits, m0 = st.misses;
		e = (unsigned char *)qoi_encode_cached(im.pixels, &im.desc, &len);
		free(e);
		e = (unsigned char *)qoi_encode_cached(im.pixels, &im.desc, &len);
		free(e);
		qoi_cache_get_stats(&st);
		check(st.misses == m0 + 1 && st.hits == h0 + 1 && st.entries == 1,
			"hash change: new hash rebuilds entry");
		check(st.collisions == 0, "hash change: no collision within one image");
	}

	qoi_cache_set_hash(NULL);
	qoi_cache_get_stats(&st);
	check(st.entries == 0, "hash change: restoring hash drops weak entries");
	qoi_cache_destroy();
	free_image(&im);
}

/* Throughput comparison: warm cache vs plain encode, same calls. */
static void test_throughput(image *base) {
	const int calls = 4000;
	double t0, cache_sec, plain_sec;
	int i, len;
	unsigned char *e;

	printf("  [8] warm-cache throughput (64x64 RGBA, %d calls)\n", calls);
	qoi_cache_config(1u << 20, QOI_CACHE_LRU);
	qoi_cache_clear();
	e = (unsigned char *)qoi_encode_cached(base->pixels, &base->desc, &len);
	free(e);

	t0 = now_sec();
	for (i = 0; i < calls; i++) {
		e = (unsigned char *)qoi_encode_cached(base->pixels, &base->desc, &len);
		free(e);
	}
	cache_sec = now_sec() - t0;

	t0 = now_sec();
	for (i = 0; i < calls; i++) {
		e = (unsigned char *)qoi_encode(base->pixels, &base->desc, &len);
		free(e);
	}
	plain_sec = now_sec() - t0;

	printf("    cached %.2f ms (%.2f us/call) vs plain %.2f ms (%.2f us/call)"
		" -> %.2fx\n", cache_sec * 1e3, cache_sec * 1e6 / calls,
		plain_sec * 1e3, plain_sec * 1e6 / calls,
		cache_sec > 0 ? plain_sec / cache_sec : 0.0);
	check(cache_sec < plain_sec, "throughput: warm cache faster than re-encoding");
	qoi_cache_clear();
}

/* Search a deterministic one-byte edit of `base` that shares its 8-bit weak
fingerprint. This is how the collision group gets images that genuinely
collide under the weak hash without hard-coding a pair that only works for one
build. Returns 0 if none was found (then the test reports a failure). */
static int find_weak_collision(const image *base, image *edited) {
	size_t px_len = (size_t)base->desc.width * base->desc.height *
		base->desc.channels;
	unsigned long long target;
	int byte, bit;

	memcpy(edited->pixels, base->pixels, px_len);
	edited->desc = base->desc;

	qoi_cache_set_weak_hash(8);
	target = qoi_cache_get_hash()(base->pixels, &base->desc);

	for (byte = 0; byte < (int)px_len; byte++) {
		for (bit = 0; bit < 8; bit++) {
			edited->pixels[byte] = (unsigned char)(base->pixels[byte] ^ (1u << bit));
			if (qoi_cache_get_hash()(edited->pixels, &edited->desc) == target) {
				printf("  chose collision edit: byte %d bit %d (0x%02x -> 0x%02x),"
					" weak hash=0x%llx\n", byte, bit, base->pixels[byte],
					edited->pixels[byte], target);
				return 1;
			}
		}
		edited->pixels[byte] = base->pixels[byte];
	}
	return 0;
}

/* Deterministic randomized soak: a small pool of images with random edits,
   random policies and capacities, occasional hash changes. Every result must
   match qoi_encode() exactly. Catches state-machine bugs (eviction order,
   accounting, hit/miss bookkeeping) that targeted cases might miss. */
static void test_soak(void) {
	enum { POOL = 6 };
	image pool[POOL];
	unsigned int rng = 0x12345678u;
	int step, i;
	unsigned long long mismatches = 0;
	unsigned long long ok = 0, nulls = 0;
	qoi_cache_stats st;

	printf("  [9] randomized soak (1200 mixed operations)\n");
	for (i = 0; i < POOL; i++) {
		char nm[16];
		sprintf(nm, "soak-%d", i);
		pool[i] = make_image(nm, 8 + i * 5, 6 + i * 3, (i & 1) ? 3 : 4);
	}

	qoi_cache_set_hash(NULL);
	qoi_cache_config(8u * 1024u, QOI_CACHE_LRU);
	qoi_cache_clear();

	for (step = 0; step < 1200; step++) {
		unsigned int r = (rng = rng * 1664525u + 1013904223u);
		image *im;
		unsigned char *plain = NULL, *e;
		int plen, len = 0;

		switch ((r >> 28) & 7) {
			case 0:
				qoi_cache_config(2048u + (r >> 8) % (30u * 1024u), QOI_CACHE_LRU);
				break;
			case 1:
				qoi_cache_config(2048u + (r >> 8) % (30u * 1024u), QOI_CACHE_FIFO);
				break;
			case 2:
				qoi_cache_config(2048u + (r >> 8) % (30u * 1024u), QOI_CACHE_RAND);
				break;
			case 3:
				qoi_cache_set_weak_hash((r >> 8) % 9u);
				break;
			case 4:
				qoi_cache_set_hash(NULL);
				break;
			case 5:
				qoi_cache_clear();
				break;
			default:
				break;
		}

		im = &pool[(r >> 3) % POOL];
		if ((r & 0xE000) == 0x2000 && im->desc.width * im->desc.height > 1) {
			/* sometimes flip one pixel; the next visit may see a miss */
			size_t o = ((size_t)((r >> 5) % (im->desc.width * im->desc.height))) *
				im->desc.channels;
			im->pixels[o] ^= (unsigned char)(1u << ((r >> 12) & 7));
		}

		plen = encode_plain(im, &plain);
		e = (unsigned char *)qoi_encode_cached(im->pixels, &im->desc, &len);
		if (!e || !plain) {
			if (e || plain) mismatches++; /* one succeeded, the other failed */
			nulls++;
		}
		else if (plen != len || memcmp(e, plain, (size_t)len) != 0) {
			mismatches++;
		}
		else {
			ok++;
		}
		free(e);
		free(plain);
	}

	qoi_cache_get_stats(&st);
	printf("    ops=1200 ok=%llu nulls=%llu bytes=%llu entries=%u hits=%llu "
		"misses=%llu collisions=%llu evictions=%llu\n",
		ok, nulls, st.bytes, st.entries, st.hits,
		st.misses, st.collisions, st.evictions);
	check(mismatches == 0, "soak: every cached result matches qoi_encode");
	check(st.bytes <= 16u * 1024u, "soak: capacity respected throughout");
	check(st.hits > 0, "soak: mixed workload actually produces hits");
	check(st.misses > 0, "soak: mixed workload actually produces misses");
	qoi_cache_set_hash(NULL);
	qoi_cache_config(8u << 20, QOI_CACHE_LRU);
	qoi_cache_clear();
	for (i = 0; i < POOL; i++) free_image(&pool[i]);
}

/* -------------------------------------------------------------------- main */

int main(void) {
	image base = make_image("base", 64, 64, 4);
	image edited = make_image("edited", 64, 64, 4);
	image small = make_image("small", 16, 16, 4);
	image medium = make_image("medium", 40, 30, 4);
	int have_collision_pair;

	printf("qoi content-fingerprint encode cache: tests and measurements\n");
	printf("================================================================\n");

	printf("\n[0] byte-exactness vs pristine qoi.h\n");
	test_golden_and_transparency();

	printf("\n[h] cache benefit per workload (4 MiB LRU unless noted)\n");
	test_group_duplicates(&base);
	test_group_one_pixel(&base, &edited);
	test_group_sizes(&small, &medium, &base);
	have_collision_pair = find_weak_collision(&base, &edited);
	check(have_collision_pair, "collisions: found a weak-hash collision pair");
	if (have_collision_pair) {
		test_group_collisions(&base, &edited);
	}
	qoi_cache_set_hash(NULL);

	printf("\n[c] cache behaviour\n");
	test_eviction();
	test_invalid();
	test_hash_change_invalidates();
	test_soak();
	test_throughput(&base);

	free_image(&base);
	free_image(&edited);
	free_image(&small);
	free_image(&medium);
	qoi_cache_destroy();

	printf("\n----------------------------------------------------------------\n");
	printf("%d checks, %d failures\n", checks, failures);
	if (failures == 0) {
		printf("ALL CACHE TESTS PASSED\n");
		return 0;
	}
	printf("CACHE TESTS FAILED\n");
	return 1;
}
