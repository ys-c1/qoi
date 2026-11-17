/*

qba_test.c - self-contained tests and benchmark for the QBA block-adaptive
extension in qoi.h. No third-party dependencies; all test images are
generated in memory.

Compile and run with:
	gcc -std=c99 -O2 -Wall -Wextra qba_test.c -o qba_test && ./qba_test

*/

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 199309L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>

#define QOI_IMPLEMENTATION
#include "qoi.h"

#define W 64
#define H 64
#define CH 4
#define NPIX (W * H)
#define RAW_BYTES (NPIX * CH)
#define ITER 2000

static int failures = 0;
static int checks = 0;

#define CHECK(cond, msg) do { \
	checks++; \
	if (!(cond)) { failures++; printf("FAIL: %s (line %d)\n", msg, __LINE__); } \
} while (0)

static unsigned char *img_flat;
static unsigned char *img_palette;
static unsigned char *img_detailed;
static unsigned char *img_mixed;

static void px_set(unsigned char *img, int x, int y,
	unsigned char r, unsigned char g, unsigned char b) {
	unsigned char *p = img + (y * W + x) * CH;
	p[0] = r; p[1] = g; p[2] = b; p[3] = 255;
}

static void make_images(void) {
	int x, y;
	img_flat = malloc(RAW_BYTES);
	img_palette = malloc(RAW_BYTES);
	img_detailed = malloc(RAW_BYTES);
	img_mixed = malloc(RAW_BYTES);

	/* 1. flat: one solid color with a few wide bands -> runs everywhere */
	for (y = 0; y < H; y++) {
		for (x = 0; x < W; x++) {
			if (y < 20) { px_set(img_flat, x, y, 10, 20, 30); }
			else if (y < 44) { px_set(img_flat, x, y, 200, 100, 50); }
			else { px_set(img_flat, x, y, 0, 255, 128); }
		}
	}

	/* 2. palette: small set of colors, no long runs -> index coding */
	for (y = 0; y < H; y++) {
		for (x = 0; x < W; x++) {
			static const unsigned char pal[8][3] = {
				{0,0,0},{255,0,0},{0,255,0},{0,0,255},
				{255,255,0},{255,0,255},{0,255,255},{255,255,255}
			};
			int k = ((x * 7 + y * 13) ^ (x * y)) & 7;
			px_set(img_palette, x, y, pal[k][0], pal[k][1], pal[k][2]);
		}
	}

	/* 3. detailed: pseudo-random per-pixel colors, > 255 distinct -> raw */
	for (y = 0; y < H; y++) {
		for (x = 0; x < W; x++) {
			unsigned int s = (unsigned int)(x * 374761393u +
				y * 668265263u + 1274126177u);
			s ^= s >> 13;
			s *= 1274126177u;
			s ^= s >> 16;
			px_set(img_detailed, x, y,
				(unsigned char)(s & 0xff),
				(unsigned char)((s >> 8) & 0xff),
				(unsigned char)((s >> 16) & 0xff));
		}
	}

	/* 4. mixed: flat top half, detailed bottom half */
	memcpy(img_mixed, img_flat, RAW_BYTES);
	memcpy(img_mixed + (H / 2) * W * CH,
		img_detailed + (H / 2) * W * CH,
		(H / 2) * W * CH);
}

static uint64_t now_ns(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

static void roundtrip_test(const char *name, const unsigned char *img,
	const qba_config *cfg) {
	qoi_desc desc = { .width = W, .height = H,
		.channels = CH, .colorspace = QOI_SRGB };
	int qba_len = 0, qoi_len = 0;
	void *qba_data, *qoi_data, *dec;
	qoi_desc ddesc;
	qba_stats st;

	qba_data = qba_encode(img, &desc, cfg, &qba_len);
	qoi_data = qoi_encode(img, &desc, &qoi_len);
	CHECK(qba_data != NULL && qba_len > 0, "qba encode");
	CHECK(qoi_data != NULL && qoi_len > 0, "qoi encode");

	memset(&st, 0xff, sizeof(st));
	dec = qba_decode(qba_data, qba_len, &ddesc, CH, &st);
	CHECK(dec != NULL, "qba decode");
	if (dec) {
		CHECK(ddesc.width == W && ddesc.height == H &&
			ddesc.channels == CH, "decoded descriptor");
		CHECK(memcmp(dec, img, RAW_BYTES) == 0,
			"lossless roundtrip pixels");
		free(dec);
	}
	CHECK(st.total_blocks ==
		(H + cfg->block_height - 1) / cfg->block_height,
		"total block count");
	CHECK(st.header_bytes ==
		st.total_blocks * QBA_BLOCK_HEADER_SIZE,
		"header bytes accounting");
	CHECK(st.corrupted_blocks == 0, "no corruption on clean stream");
	{
		unsigned int sum = st.mode_counts[0] + st.mode_counts[1] +
			st.mode_counts[2];
		CHECK(sum == st.total_blocks, "mode counts cover all blocks");
	}

	/* decode with channels == 0 (use stream's channel count) */
	dec = qba_decode(qba_data, qba_len, &ddesc, 0, NULL);
	CHECK(dec != NULL && memcmp(dec, img, RAW_BYTES) == 0,
		"decode with channels=0");
	free(dec);

	printf("  %-9s qba=%5d B  qoi=%5d B  ratio/qoi=%5.1f%%  ratio/raw=%5.1f%%"
		"  hdr=%3u B (%4.1f%%)  modes r/i/r=%u/%u/%u\n",
		name, qba_len, qoi_len,
		100.0 * qba_len / qoi_len,
		100.0 * qba_len / RAW_BYTES,
		st.header_bytes, 100.0 * st.header_bytes / qba_len,
		st.mode_counts[QBA_MODE_RLE],
		st.mode_counts[QBA_MODE_INDEX],
		st.mode_counts[QBA_MODE_RAW]);

	free(qba_data);
	free(qoi_data);
}

static void bench_decode(const char *name, const unsigned char *img,
	const qba_config *cfg) {
	qoi_desc desc = { .width = W, .height = H,
		.channels = CH, .colorspace = QOI_SRGB };
	int qba_len = 0, qoi_len = 0, i;
	void *qba_data, *qoi_data, *dec;
	uint64_t t0, t1;
	double qba_ms, qoi_ms;

	qba_data = qba_encode(img, &desc, cfg, &qba_len);
	qoi_data = qoi_encode(img, &desc, &qoi_len);

	t0 = now_ns();
	for (i = 0; i < ITER; i++) {
		dec = qba_decode(qba_data, qba_len, &desc, CH, NULL);
		free(dec);
	}
	t1 = now_ns();
	qba_ms = (t1 - t0) / 1e6;

	t0 = now_ns();
	for (i = 0; i < ITER; i++) {
		dec = qoi_decode(qoi_data, qoi_len, &desc, CH);
		free(dec);
	}
	t1 = now_ns();
	qoi_ms = (t1 - t0) / 1e6;

	printf("  %-9s decode x%d: qba=%7.2f ms (%5.1f ns/px)  "
		"qoi=%7.2f ms (%5.1f ns/px)\n",
		name, ITER, qba_ms, qba_ms * 1e6 / (ITER * NPIX),
		qoi_ms, qoi_ms * 1e6 / (ITER * NPIX));

	free(qba_data);
	free(qoi_data);
}

static void config_test(void) {
	qoi_desc desc = { .width = W, .height = H,
		.channels = CH, .colorspace = QOI_SRGB };
	qba_config cfg;
	int len;
	void *data, *dec;
	qoi_desc ddesc;

	qba_default_config(&cfg);
	CHECK(cfg.block_height == 16 && cfg.rle_threshold == 4 &&
		cfg.index_colors_threshold == 32, "default config values");

	/* block height 8: H=64 -> exactly 8 blocks */
	cfg.block_height = 8;
	data = qba_encode(img_mixed, &desc, &cfg, &len);
	CHECK(data != NULL, "encode block_height=8");
	dec = qba_decode(data, len, &ddesc, CH, NULL);
	CHECK(dec != NULL && memcmp(dec, img_mixed, RAW_BYTES) == 0,
		"roundtrip block_height=8");
	free(dec);
	free(data);

	/* non-divisor height: block height 20 -> last block is a partial */
	cfg.block_height = 20;
	data = qba_encode(img_palette, &desc, &cfg, &len);
	CHECK(data != NULL, "encode block_height=20 (partial last block)");
	dec = qba_decode(data, len, &ddesc, CH, NULL);
	CHECK(dec != NULL && memcmp(dec, img_palette, RAW_BYTES) == 0,
		"roundtrip partial last block");
	free(dec);
	free(data);

	/* threshold tuning: rle_threshold=1 forces rle on any repeat */
	cfg.block_height = 16;
	cfg.rle_threshold = 1;
	cfg.index_colors_threshold = 1;
	data = qba_encode(img_flat, &desc, &cfg, &len);
	{
		qba_stats st;
		dec = qba_decode(data, len, &ddesc, CH, &st);
		CHECK(dec != NULL && memcmp(dec, img_flat, RAW_BYTES) == 0,
			"roundtrip aggressive rle thresholds");
		CHECK(st.mode_counts[QBA_MODE_RLE] == st.total_blocks,
			"rle_threshold=1 selects rle for flat blocks");
		free(dec);
	}
	free(data);

	/* invalid configs rejected */
	cfg = (qba_config){ .block_height = 0, .rle_threshold = 4,
		.index_colors_threshold = 32 };
	CHECK(qba_encode(img_flat, &desc, &cfg, &len) == NULL,
		"reject block_height=0");
	cfg = (qba_config){ .block_height = 16, .rle_threshold = 4,
		.index_colors_threshold = 256 };
	CHECK(qba_encode(img_flat, &desc, &cfg, &len) == NULL,
		"reject index threshold > 255");

	/* invalid arguments */
	CHECK(qba_encode(NULL, &desc, NULL, &len) == NULL, "null data");
	CHECK(qba_decode("x", 1, &ddesc, CH, NULL) == NULL, "truncated input");
}

static void corruption_test(void) {
	qoi_desc desc = { .width = W, .height = H,
		.channels = CH, .colorspace = QOI_SRGB };
	qba_config cfg;
	int len;
	unsigned char *stream;
	void *data, *dec;
	qoi_desc ddesc;
	qba_stats st;

	qba_default_config(&cfg); /* block height 16 -> 4 blocks */
	data = qba_encode(img_mixed, &desc, &cfg, &len);
	CHECK(data != NULL, "corruption test encode");
	stream = malloc(len);

	/* 1. flip the mode byte of block 1's header. Block positions are
	      variable (payload lengths differ), so walk the stream to find
	      block number 1's header offset. CRC must fail, block 1 is
	      skipped, earlier and later blocks still decode. */
	memcpy(stream, data, len);
	{
		int walk = QBA_CONTAINER_HEADER_SIZE;
		unsigned int blocknum = 0;
		while (blocknum < 1) {
			unsigned int plen =
				((unsigned int)stream[walk + 5] << 24) |
				((unsigned int)stream[walk + 6] << 16) |
				((unsigned int)stream[walk + 7] << 8) |
				stream[walk + 8];
			blocknum = ((unsigned int)stream[walk + 3] << 8) |
				stream[walk + 4];
			if (blocknum == 1) { break; }
			walk += QBA_BLOCK_HEADER_SIZE + plen;
		}
		stream[walk + 2] ^= 0x80; /* mode byte of block 1 */
	}
	memset(&st, 0, sizeof(st));
	dec = qba_decode(stream, len, &ddesc, CH, &st);
	CHECK(dec != NULL, "decode after header corruption");
	if (dec) {
		unsigned char *out = dec;
		int y;
		int block1_bad = 1, other_ok = 1;
		for (y = 0; y < H; y++) {
			int is_block1 = (y >= 16 && y < 32);
			int match = memcmp(out + y * W * CH,
				img_mixed + y * W * CH, W * CH) == 0;
			if (is_block1) {
				int sentinel = (out[(y * W) * CH] == 255 &&
					out[(y * W) * CH + 1] == 0 &&
					out[(y * W) * CH + 2] == 255);
				if (!sentinel) { block1_bad = 0; }
			}
			else if (!match) {
				other_ok = 0;
			}
		}
		CHECK(block1_bad, "corrupt block 1 filled with sentinel");
		CHECK(other_ok, "blocks 0,2,3 still decoded correctly");
		free(dec);
	}
	CHECK(st.corrupted_blocks == 1, "exactly one corrupt block reported");

	/* 1b. corrupt the very first block header too: block 0 becomes
	      sentinel, the stream still resynchronizes at block 1. */
	memcpy(stream, data, len);
	stream[QBA_CONTAINER_HEADER_SIZE + 3] ^= 0xff; /* block 0 number */
	{
		qba_stats st0;
		int y, first_sentinel = 1, rest_ok = 1;
		memset(&st0, 0, sizeof(st0));
		dec = qba_decode(stream, len, &ddesc, CH, &st0);
		CHECK(dec != NULL && st0.corrupted_blocks == 1,
			"first-block header corruption detected");
		if (dec) {
			unsigned char *o = dec;
			for (y = 0; y < 16; y++) {
				if (!(o[(y * W) * CH] == 255 &&
					o[(y * W) * CH + 1] == 0 &&
					o[(y * W) * CH + 2] == 255)) {
					first_sentinel = 0;
				}
			}
			for (y = 16; y < H; y++) {
				if (memcmp(o + y * W * CH, img_mixed + y * W * CH,
					W * CH) != 0) {
					rest_ok = 0;
				}
			}
			CHECK(first_sentinel, "block 0 sentinel after corruption");
			CHECK(rest_ok, "blocks 1..3 recovered after block 0 loss");
			free(dec);
		}
	}

	/* 2. random single-byte mutations across the whole stream must never
	      crash or return NULL; run them all. */
	{
		int i, hard_failures = 0;
		for (i = QBA_CONTAINER_HEADER_SIZE; i < len; i++) {
			unsigned char save;
			memcpy(stream, data, len);
			save = stream[i];
			stream[i] = (unsigned char)(save ^ 0x5a);
			dec = qba_decode(stream, len, &ddesc, CH, NULL);
			if (!dec) { hard_failures++; }
			else { free(dec); }
		}
		CHECK(hard_failures == 0, "all byte mutations handled gracefully");
	}

	/* 3. garbage appended / truncated streams */
	{
		unsigned char *garbage = malloc(len + 64);
		memcpy(garbage, data, len);
		memset(garbage + len, 0xab, 64);
		dec = qba_decode(garbage, len + 64, &ddesc, CH, NULL);
		CHECK(dec != NULL &&
			memcmp(dec, img_mixed, RAW_BYTES) == 0,
			"trailing garbage ignored");
		free(dec);
		dec = qba_decode(garbage, QBA_CONTAINER_HEADER_SIZE + 3,
			&ddesc, CH, NULL);
		CHECK(dec == NULL, "truncated stream rejected");
		free(garbage);
	}

	free(stream);
	free(data);
}

static void rgb_test(void) {
	const int w = 48, h = 40, ch3 = 3, ch4 = 4;
	int npix = w * h;
	unsigned char *rgb = malloc(npix * ch3);
	unsigned char *rgba = malloc(npix * ch4);
	qoi_desc desc3 = { .width = w, .height = h,
		.channels = ch3, .colorspace = QOI_SRGB };
	qba_config cfg;
	int len, i, x, y;
	void *data, *dec;
	qoi_desc ddesc;
	qba_stats st;

	qba_default_config(&cfg);
	for (y = 0; y < h; y++) {
		for (x = 0; x < w; x++) {
			int k = (x * 3 + y * 5 + (x ^ y)) & 31;
			rgb[(y * w + x) * 3 + 0] = (unsigned char)(k * 7);
			rgb[(y * w + x) * 3 + 1] = (unsigned char)(255 - k * 5);
			rgb[(y * w + x) * 3 + 2] = (unsigned char)(k * 11);
		}
	}

	/* 3-channel encode, decode to 3 channels */
	data = qba_encode(rgb, &desc3, &cfg, &len);
	CHECK(data != NULL, "rgb encode");
	dec = qba_decode(data, len, &ddesc, ch3, &st);
	CHECK(dec != NULL && ddesc.channels == 3, "rgb decode descriptor");
	CHECK(dec && memcmp(dec, rgb, npix * 3) == 0,
		"rgb lossless roundtrip");
	CHECK(dec && st.corrupted_blocks == 0, "rgb clean stats");
	free(dec);

	/* decode the same 3-channel stream forcing 4-channel output, with
	   alpha filled to 255 */
	dec = qba_decode(data, len, &ddesc, ch4, NULL);
	CHECK(dec != NULL, "rgb stream decoded to rgba");
	if (dec) {
		int ok = 1;
		for (i = 0; i < npix; i++) {
			unsigned char *o = (unsigned char *)dec + i * 4;
			unsigned char *s = rgb + i * 3;
			if (o[0] != s[0] || o[1] != s[1] || o[2] != s[2] ||
				o[3] != 255) {
				ok = 0;
				break;
			}
		}
		CHECK(ok, "rgb->rgba conversion fills alpha=255");
		free(dec);
	}
	free(data);

	/* 4-channel source encoded, then decoded to 3 channels (alpha dropped) */
	for (i = 0; i < npix; i++) {
		rgba[i * 4 + 0] = rgb[i * 3 + 0];
		rgba[i * 4 + 1] = rgb[i * 3 + 1];
		rgba[i * 4 + 2] = rgb[i * 3 + 2];
		rgba[i * 4 + 3] = (unsigned char)(128 + (i & 127));
	}
	{
		qoi_desc desc4 = { .width = w, .height = h,
			.channels = ch4, .colorspace = QOI_SRGB };
		data = qba_encode(rgba, &desc4, &cfg, &len);
		CHECK(data != NULL, "rgba encode");
		dec = qba_decode(data, len, &ddesc, ch3, NULL);
		CHECK(dec != NULL, "rgba stream decoded to rgb");
		CHECK(dec && memcmp(dec, rgb, npix * 3) == 0,
			"rgba->rgb drops alpha");
		free(dec);
		free(data);
	}

	free(rgb);
	free(rgba);
}

int main(void) {
	qba_config cfg;
	qba_default_config(&cfg);

	make_images();

	printf("== QBA size comparison (image %dx%d, block_height=%u) ==\n",
		W, H, cfg.block_height);
	roundtrip_test("flat", img_flat, &cfg);
	roundtrip_test("palette", img_palette, &cfg);
	roundtrip_test("detailed", img_detailed, &cfg);
	roundtrip_test("mixed", img_mixed, &cfg);

	printf("\n== RGB (3-channel) streams and channel conversion ==\n");
	rgb_test();

	printf("\n== decode timing (single-thread, %d iterations) ==\n", ITER);
	bench_decode("flat", img_flat, &cfg);
	bench_decode("palette", img_palette, &cfg);
	bench_decode("detailed", img_detailed, &cfg);
	bench_decode("mixed", img_mixed, &cfg);

	printf("\n== configuration / edge cases ==\n");
	config_test();

	printf("\n== block header corruption / resync ==\n");
	corruption_test();

	printf("\n%d checks, %d failures\n", checks, failures);
	free(img_flat); free(img_palette);
	free(img_detailed); free(img_mixed);
	return failures ? 1 : 0;
}
