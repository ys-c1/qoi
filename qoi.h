/*

Copyright (c) 2021, Dominic Szablewski - https://phoboslab.org
SPDX-License-Identifier: MIT


QOI - The "Quite OK Image" format for fast, lossless image compression

-- About

QOI encodes and decodes images in a lossless format. Compared to stb_image and
stb_image_write QOI offers 20x-50x faster encoding, 3x-4x faster decoding and
20% better compression.


-- Synopsis

// Define `QOI_IMPLEMENTATION` in *one* C/C++ file before including this
// library to create the implementation.

#define QOI_IMPLEMENTATION
#include "qoi.h"

// Encode and store an RGBA buffer to the file system. The qoi_desc describes
// the input pixel data.
qoi_write("image_new.qoi", rgba_pixels, &(qoi_desc){
	.width = 1920,
	.height = 1080,
	.channels = 4,
	.colorspace = QOI_SRGB
});

// Load and decode a QOI image from the file system into a 32bbp RGBA buffer.
// The qoi_desc struct will be filled with the width, height, number of channels
// and colorspace read from the file header.
qoi_desc desc;
void *rgba_pixels = qoi_read("image.qoi", &desc, 4);

// Optional: cache encoding results by content fingerprint, so encoding the
// same pixels repeatedly returns the previous bytes without re-encoding. The
// cached bytes are identical to qoi_encode()'s; see "Encode Result Cache"
// below. Free() the result, same as qoi_encode().
int encoded_len;
void *encoded = qoi_encode_cached(rgba_pixels, &desc, &encoded_len);



-- Documentation

This library provides the following functions;
- qoi_read    -- read and decode a QOI file
- qoi_decode  -- decode the raw bytes of a QOI image from memory
- qoi_write   -- encode and write a QOI file
- qoi_encode  -- encode an rgba buffer into a QOI image in memory
- qoi_encode_cached -- qoi_encode, with a content-fingerprint result cache

See the function declaration below for the signature and more information.

If you don't want/need the qoi_read and qoi_write functions, you can define
QOI_NO_STDIO before including this library.

This library uses malloc() and free(). To supply your own malloc implementation
you can define QOI_MALLOC and QOI_FREE before including this library.

This library uses memset() to zero-initialize the index. To supply your own
implementation you can define QOI_ZEROARR before including this library.


-- Data Format

A QOI file has a 14 byte header, followed by any number of data "chunks" and an
8-byte end marker.

struct qoi_header_t {
	char     magic[4];   // magic bytes "qoif"
	uint32_t width;      // image width in pixels (BE)
	uint32_t height;     // image height in pixels (BE)
	uint8_t  channels;   // 3 = RGB, 4 = RGBA
	uint8_t  colorspace; // 0 = sRGB with linear alpha, 1 = all channels linear
};

Images are encoded row by row, left to right, top to bottom. The decoder and
encoder start with {r: 0, g: 0, b: 0, a: 255} as the previous pixel value. An
image is complete when all pixels specified by width * height have been covered.

Pixels are encoded as
 - a run of the previous pixel
 - an index into an array of previously seen pixels
 - a difference to the previous pixel value in r,g,b
 - full r,g,b or r,g,b,a values

The color channels are assumed to not be premultiplied with the alpha channel
("un-premultiplied alpha").

A running array[64] (zero-initialized) of previously seen pixel values is
maintained by the encoder and decoder. Each pixel that is seen by the encoder
and decoder is put into this array at the position formed by a hash function of
the color value. In the encoder, if the pixel value at the index matches the
current pixel, this index position is written to the stream as QOI_OP_INDEX.
The hash function for the index is:

	index_position = (r * 3 + g * 5 + b * 7 + a * 11) % 64

Each chunk starts with a 2- or 8-bit tag, followed by a number of data bits. The
bit length of chunks is divisible by 8 - i.e. all chunks are byte aligned. All
values encoded in these data bits have the most significant bit on the left.

The 8-bit tags have precedence over the 2-bit tags. A decoder must check for the
presence of an 8-bit tag first.

The byte stream's end is marked with 7 0x00 bytes followed a single 0x01 byte.


The possible chunks are:


.- QOI_OP_INDEX ----------.
|         Byte[0]         |
|  7  6  5  4  3  2  1  0 |
|-------+-----------------|
|  0  0 |     index       |
`-------------------------`
2-bit tag b00
6-bit index into the color index array: 0..63

A valid encoder must not issue 2 or more consecutive QOI_OP_INDEX chunks to the
same index. QOI_OP_RUN should be used instead.


.- QOI_OP_DIFF -----------.
|         Byte[0]         |
|  7  6  5  4  3  2  1  0 |
|-------+-----+-----+-----|
|  0  1 |  dr |  dg |  db |
`-------------------------`
2-bit tag b01
2-bit   red channel difference from the previous pixel between -2..1
2-bit green channel difference from the previous pixel between -2..1
2-bit  blue channel difference from the previous pixel between -2..1

The difference to the current channel values are using a wraparound operation,
so "1 - 2" will result in 255, while "255 + 1" will result in 0.

Values are stored as unsigned integers with a bias of 2. E.g. -2 is stored as
0 (b00). 1 is stored as 3 (b11).

The alpha value remains unchanged from the previous pixel.


.- QOI_OP_LUMA -------------------------------------.
|         Byte[0]         |         Byte[1]         |
|  7  6  5  4  3  2  1  0 |  7  6  5  4  3  2  1  0 |
|-------+-----------------+-------------+-----------|
|  1  0 |  green diff     |   dr - dg   |  db - dg  |
`---------------------------------------------------`
2-bit tag b10
6-bit green channel difference from the previous pixel -32..31
4-bit   red channel difference minus green channel difference -8..7
4-bit  blue channel difference minus green channel difference -8..7

The green channel is used to indicate the general direction of change and is
encoded in 6 bits. The red and blue channels (dr and db) base their diffs off
of the green channel difference and are encoded in 4 bits. I.e.:
	dr_dg = (cur_px.r - prev_px.r) - (cur_px.g - prev_px.g)
	db_dg = (cur_px.b - prev_px.b) - (cur_px.g - prev_px.g)

The difference to the current channel values are using a wraparound operation,
so "10 - 13" will result in 253, while "250 + 7" will result in 1.

Values are stored as unsigned integers with a bias of 32 for the green channel
and a bias of 8 for the red and blue channel.

The alpha value remains unchanged from the previous pixel.


.- QOI_OP_RUN ------------.
|         Byte[0]         |
|  7  6  5  4  3  2  1  0 |
|-------+-----------------|
|  1  1 |       run       |
`-------------------------`
2-bit tag b11
6-bit run-length repeating the previous pixel: 1..62

The run-length is stored with a bias of -1. Note that the run-lengths 63 and 64
(b111110 and b111111) are illegal as they are occupied by the QOI_OP_RGB and
QOI_OP_RGBA tags.


.- QOI_OP_RGB ------------------------------------------.
|         Byte[0]         | Byte[1] | Byte[2] | Byte[3] |
|  7  6  5  4  3  2  1  0 | 7 .. 0  | 7 .. 0  | 7 .. 0  |
|-------------------------+---------+---------+---------|
|  1  1  1  1  1  1  1  0 |   red   |  green  |  blue   |
`-------------------------------------------------------`
8-bit tag b11111110
8-bit   red channel value
8-bit green channel value
8-bit  blue channel value

The alpha value remains unchanged from the previous pixel.


.- QOI_OP_RGBA ---------------------------------------------------.
|         Byte[0]         | Byte[1] | Byte[2] | Byte[3] | Byte[4] |
|  7  6  5  4  3  2  1  0 | 7 .. 0  | 7 .. 0  | 7 .. 0  | 7 .. 0  |
|-------------------------+---------+---------+---------+---------|
|  1  1  1  1  1  1  1  1 |   red   |  green  |  blue   |  alpha  |
`-----------------------------------------------------------------`
8-bit tag b11111111
8-bit   red channel value
8-bit green channel value
8-bit  blue channel value
8-bit alpha channel value

*/


/* -----------------------------------------------------------------------------
Header - Public functions */

#ifndef QOI_H
#define QOI_H

#ifdef __cplusplus
extern "C" {
#endif

/* A pointer to a qoi_desc struct has to be supplied to all of qoi's functions.
It describes either the input format (for qoi_write and qoi_encode), or is
filled with the description read from the file header (for qoi_read and
qoi_decode).

The colorspace in this qoi_desc is an enum where
	0 = sRGB, i.e. gamma scaled RGB channels and a linear alpha channel
	1 = all channels are linear
You may use the constants QOI_SRGB or QOI_LINEAR. The colorspace is purely
informative. It will be saved to the file header, but does not affect
how chunks are en-/decoded. */

#define QOI_SRGB   0
#define QOI_LINEAR 1

typedef struct {
	unsigned int width;
	unsigned int height;
	unsigned char channels;
	unsigned char colorspace;
} qoi_desc;

#ifndef QOI_NO_STDIO

/* Encode raw RGB or RGBA pixels into a QOI image and write it to the file
system. The qoi_desc struct must be filled with the image width, height,
number of channels (3 = RGB, 4 = RGBA) and the colorspace.

The function returns 0 on failure (invalid parameters, or fopen or malloc
failed) or the number of bytes written on success. */

int qoi_write(const char *filename, const void *data, const qoi_desc *desc);


/* Read and decode a QOI image from the file system. If channels is 0, the
number of channels from the file header is used. If channels is 3 or 4 the
output format will be forced into this number of channels.

The function either returns NULL on failure (invalid data, or malloc or fopen
failed) or a pointer to the decoded pixels. On success, the qoi_desc struct
will be filled with the description from the file header.

The returned pixel data should be free()d after use. */

void *qoi_read(const char *filename, qoi_desc *desc, int channels);

#endif /* QOI_NO_STDIO */


/* Encode raw RGB or RGBA pixels into a QOI image in memory.

The function either returns NULL on failure (invalid parameters or malloc
failed) or a pointer to the encoded data on success. On success the out_len
is set to the size in bytes of the encoded data.

The returned qoi data should be free()d after use. */

void *qoi_encode(const void *data, const qoi_desc *desc, int *out_len);


/* Decode a QOI image from memory.

The function either returns NULL on failure (invalid parameters or malloc
failed) or a pointer to the decoded pixels. On success, the qoi_desc struct
is filled with the description from the file header.

The returned pixel data should be free()d after use. */

void *qoi_decode(const void *data, int size, qoi_desc *desc, int channels);


/* -----------------------------------------------------------------------------
Encode Result Cache (content fingerprint cache)

The cache sits in front of qoi_encode() without changing it: a lookup that hits
returns a copy of the bytes qoi_encode() produced earlier, a miss calls the
original qoi_encode() and stores its result. The encoded bytes are never
modified, so caching is byte-for-byte transparent to callers.

Keys are content fingerprints: a 64-bit hash over the *pixel bytes* plus the
encoding-relevant header fields (width, height, channels, colorspace). The hash
function is replaceable at runtime with qoi_cache_set_hash(), and
qoi_cache_set_weak_hash() installs a deliberately weak (bit-folded) variant for
construction of collisions in tests and stress tools.

Hash collisions are not treated as hits blindly: every cache entry also stores
the full original pixel buffer (width*height*channels bytes) and the desc, and a
candidate match is only reused after a byte-for-byte comparison of all of that.
So a weak/forged hash can only cost speed (extra comparisons, evictions), never
correctness.

The cache is per-process (one global store), fixed capacity in bytes, with
pluggable eviction (LRU, FIFO, random). Capacity is measured in accounted bytes
(pixels copy + encoded bytes + per-entry overhead), so it bounds the cache's
heap use; entries larger than the whole capacity are not cached.

Default capacity/eviction come from QOI_CACHE_DEFAULT_CAPACITY /
QOI_CACHE_DEFAULT_POLICY; qoi_cache_config() can change them at any time.

All cached data is deep-copied, and hits hand the caller a fresh buffer - free()
it just like the result of qoi_encode(). The cache is not thread-safe; guard it
with your own lock if several threads share it. */

/* hash: 64-bit digest of pixels + desc fields. */
typedef unsigned long long (*qoi_cache_hash_fn)(const void *data, const qoi_desc *desc);

/* Eviction policies. LRU is the default. */
#define QOI_CACHE_LRU   0
#define QOI_CACHE_FIFO  1
#define QOI_CACHE_RAND  2

/* Defaults; override at compile time if you like. */
#ifndef QOI_CACHE_DEFAULT_CAPACITY
	#define QOI_CACHE_DEFAULT_CAPACITY (8u * 1024u * 1024u) /* bytes */
#endif
#ifndef QOI_CACHE_DEFAULT_POLICY
	#define QOI_CACHE_DEFAULT_POLICY QOI_CACHE_LRU
#endif

/* Statistics. `hits` counts lookups returned from the cache, `misses` counts
lookups that fell through to qoi_encode() (including collisions that could not
be used), `collisions` counts fingerprint collisions detected during lookup,
`evictions` counts entries discarded to make room or by reconfiguration,
`stores` counts successful insertions, `bytes` is the accounted cache size. */
typedef struct {
	unsigned long long hits;
	unsigned long long misses;
	unsigned long long collisions;
	unsigned long long evictions;
	unsigned long long stores;
	unsigned long long bytes;
	unsigned int entries;
} qoi_cache_stats;

/* Configure capacity (accounted bytes; 0 disables storing) and eviction policy.
May be called at any time; shrinking drops entries until the limit fits.
Unknown policies are ignored. */
void qoi_cache_config(unsigned int capacity_bytes, int policy);

/* Replace the hash function, e.g. with a weak one to construct collisions.
0 restores the built-in 64-bit hash. Any change drops all entries, because
stored fingerprints only mean something for the hash that made them. */
void qoi_cache_set_hash(qoi_cache_hash_fn fn);
qoi_cache_hash_fn qoi_cache_get_hash(void);

/* Deliberately weak hash: the built-in 64-bit hash folded down to only `bits`
bits (0..64). Installing it clears the cache. It lets tests and stress tools
construct collisions on purpose; correctness is preserved by full content
verification either way. */
unsigned long long qoi_cache_weak_hash(const void *data, const qoi_desc *desc);
void qoi_cache_set_weak_hash(unsigned int bits);

/* Look up/insert through the cache. Returns a freshly malloc'd buffer holding
exactly the bytes qoi_encode() would return, with *out_len set; caller frees.
Returns NULL if qoi_encode() itself would fail (invalid args/malloc), in which
case nothing is stored. */
void *qoi_encode_cached(const void *data, const qoi_desc *desc, int *out_len);

/* Read current statistics (never reset by this call). */
void qoi_cache_get_stats(qoi_cache_stats *out);

/* Drop all entries (keeps configuration) and reset the statistics window. */
void qoi_cache_clear(void);

/* Free the cache's own bookkeeping. Safe to call repeatedly. */
void qoi_cache_destroy(void);


#ifdef __cplusplus
}
#endif
#endif /* QOI_H */


/* -----------------------------------------------------------------------------
Implementation */

#ifdef QOI_IMPLEMENTATION
#include <stdlib.h>
#include <string.h>

#ifndef QOI_MALLOC
	#define QOI_MALLOC(sz) malloc(sz)
	#define QOI_FREE(p)    free(p)
#endif
#ifndef QOI_ZEROARR
	#define QOI_ZEROARR(a) memset((a),0,sizeof(a))
#endif

#define QOI_OP_INDEX  0x00 /* 00xxxxxx */
#define QOI_OP_DIFF   0x40 /* 01xxxxxx */
#define QOI_OP_LUMA   0x80 /* 10xxxxxx */
#define QOI_OP_RUN    0xc0 /* 11xxxxxx */
#define QOI_OP_RGB    0xfe /* 11111110 */
#define QOI_OP_RGBA   0xff /* 11111111 */

#define QOI_MASK_2    0xc0 /* 11000000 */

#define QOI_COLOR_HASH(C) (C.rgba.r*3 + C.rgba.g*5 + C.rgba.b*7 + C.rgba.a*11)
#define QOI_MAGIC \
	(((unsigned int)'q') << 24 | ((unsigned int)'o') << 16 | \
	 ((unsigned int)'i') <<  8 | ((unsigned int)'f'))
#define QOI_HEADER_SIZE 14

/* 2GB is the max file size that this implementation can safely handle. We guard
against anything larger than that, assuming the worst case with 5 bytes per
pixel, rounded down to a nice clean value. 400 million pixels ought to be
enough for anybody. */
#define QOI_PIXELS_MAX ((unsigned int)400000000)

typedef union {
	struct { unsigned char r, g, b, a; } rgba;
	unsigned int v;
} qoi_rgba_t;

static const unsigned char qoi_padding[8] = {0,0,0,0,0,0,0,1};

static void qoi_write_32(unsigned char *bytes, int *p, unsigned int v) {
	bytes[(*p)++] = (0xff000000 & v) >> 24;
	bytes[(*p)++] = (0x00ff0000 & v) >> 16;
	bytes[(*p)++] = (0x0000ff00 & v) >> 8;
	bytes[(*p)++] = (0x000000ff & v);
}

static unsigned int qoi_read_32(const unsigned char *bytes, int *p) {
	unsigned int a = bytes[(*p)++];
	unsigned int b = bytes[(*p)++];
	unsigned int c = bytes[(*p)++];
	unsigned int d = bytes[(*p)++];
	return a << 24 | b << 16 | c << 8 | d;
}

void *qoi_encode(const void *data, const qoi_desc *desc, int *out_len) {
	int i, max_size, p, run;
	int px_len, px_end, px_pos, channels;
	unsigned char *bytes;
	const unsigned char *pixels;
	qoi_rgba_t index[64];
	qoi_rgba_t px, px_prev;

	if (
		data == NULL || out_len == NULL || desc == NULL ||
		desc->width == 0 || desc->height == 0 ||
		desc->channels < 3 || desc->channels > 4 ||
		desc->colorspace > 1 ||
		desc->height >= QOI_PIXELS_MAX / desc->width
	) {
		return NULL;
	}

	max_size =
		desc->width * desc->height * (desc->channels + 1) +
		QOI_HEADER_SIZE + sizeof(qoi_padding);

	p = 0;
	bytes = (unsigned char *) QOI_MALLOC(max_size);
	if (!bytes) {
		return NULL;
	}

	qoi_write_32(bytes, &p, QOI_MAGIC);
	qoi_write_32(bytes, &p, desc->width);
	qoi_write_32(bytes, &p, desc->height);
	bytes[p++] = desc->channels;
	bytes[p++] = desc->colorspace;


	pixels = (const unsigned char *)data;

	QOI_ZEROARR(index);

	run = 0;
	px_prev.rgba.r = 0;
	px_prev.rgba.g = 0;
	px_prev.rgba.b = 0;
	px_prev.rgba.a = 255;
	px = px_prev;

	px_len = desc->width * desc->height * desc->channels;
	px_end = px_len - desc->channels;
	channels = desc->channels;

	for (px_pos = 0; px_pos < px_len; px_pos += channels) {
		px.rgba.r = pixels[px_pos + 0];
		px.rgba.g = pixels[px_pos + 1];
		px.rgba.b = pixels[px_pos + 2];

		if (channels == 4) {
			px.rgba.a = pixels[px_pos + 3];
		}

		if (px.v == px_prev.v) {
			run++;
			if (run == 62 || px_pos == px_end) {
				bytes[p++] = QOI_OP_RUN | (run - 1);
				run = 0;
			}
		}
		else {
			int index_pos;

			if (run > 0) {
				bytes[p++] = QOI_OP_RUN | (run - 1);
				run = 0;
			}

			index_pos = QOI_COLOR_HASH(px) & (64 - 1);

			if (index[index_pos].v == px.v) {
				bytes[p++] = QOI_OP_INDEX | index_pos;
			}
			else {
				index[index_pos] = px;

				if (px.rgba.a == px_prev.rgba.a) {
					signed char vr = px.rgba.r - px_prev.rgba.r;
					signed char vg = px.rgba.g - px_prev.rgba.g;
					signed char vb = px.rgba.b - px_prev.rgba.b;

					signed char vg_r = vr - vg;
					signed char vg_b = vb - vg;

					if (
						vr > -3 && vr < 2 &&
						vg > -3 && vg < 2 &&
						vb > -3 && vb < 2
					) {
						bytes[p++] = QOI_OP_DIFF | (vr + 2) << 4 | (vg + 2) << 2 | (vb + 2);
					}
					else if (
						vg_r >  -9 && vg_r <  8 &&
						vg   > -33 && vg   < 32 &&
						vg_b >  -9 && vg_b <  8
					) {
						bytes[p++] = QOI_OP_LUMA     | (vg   + 32);
						bytes[p++] = (vg_r + 8) << 4 | (vg_b +  8);
					}
					else {
						bytes[p++] = QOI_OP_RGB;
						bytes[p++] = px.rgba.r;
						bytes[p++] = px.rgba.g;
						bytes[p++] = px.rgba.b;
					}
				}
				else {
					bytes[p++] = QOI_OP_RGBA;
					bytes[p++] = px.rgba.r;
					bytes[p++] = px.rgba.g;
					bytes[p++] = px.rgba.b;
					bytes[p++] = px.rgba.a;
				}
			}
		}
		px_prev = px;
	}

	for (i = 0; i < (int)sizeof(qoi_padding); i++) {
		bytes[p++] = qoi_padding[i];
	}

	*out_len = p;
	return bytes;
}

void *qoi_decode(const void *data, int size, qoi_desc *desc, int channels) {
	const unsigned char *bytes;
	unsigned int header_magic;
	unsigned char *pixels;
	qoi_rgba_t index[64];
	qoi_rgba_t px;
	int px_len, chunks_len, px_pos;
	int p = 0, run = 0;

	if (
		data == NULL || desc == NULL ||
		(channels != 0 && channels != 3 && channels != 4) ||
		size < QOI_HEADER_SIZE + (int)sizeof(qoi_padding)
	) {
		return NULL;
	}

	bytes = (const unsigned char *)data;

	header_magic = qoi_read_32(bytes, &p);
	desc->width = qoi_read_32(bytes, &p);
	desc->height = qoi_read_32(bytes, &p);
	desc->channels = bytes[p++];
	desc->colorspace = bytes[p++];

	if (
		desc->width == 0 || desc->height == 0 ||
		desc->channels < 3 || desc->channels > 4 ||
		desc->colorspace > 1 ||
		header_magic != QOI_MAGIC ||
		desc->height >= QOI_PIXELS_MAX / desc->width
	) {
		return NULL;
	}

	if (channels == 0) {
		channels = desc->channels;
	}

	px_len = desc->width * desc->height * channels;
	pixels = (unsigned char *) QOI_MALLOC(px_len);
	if (!pixels) {
		return NULL;
	}

	QOI_ZEROARR(index);
	px.rgba.r = 0;
	px.rgba.g = 0;
	px.rgba.b = 0;
	px.rgba.a = 255;

	chunks_len = size - (int)sizeof(qoi_padding);
	for (px_pos = 0; px_pos < px_len; px_pos += channels) {
		if (run > 0) {
			run--;
		}
		else if (p < chunks_len) {
			int b1 = bytes[p++];

			if (b1 == QOI_OP_RGB) {
				px.rgba.r = bytes[p++];
				px.rgba.g = bytes[p++];
				px.rgba.b = bytes[p++];
			}
			else if (b1 == QOI_OP_RGBA) {
				px.rgba.r = bytes[p++];
				px.rgba.g = bytes[p++];
				px.rgba.b = bytes[p++];
				px.rgba.a = bytes[p++];
			}
			else if ((b1 & QOI_MASK_2) == QOI_OP_INDEX) {
				px = index[b1];
			}
			else if ((b1 & QOI_MASK_2) == QOI_OP_DIFF) {
				px.rgba.r += ((b1 >> 4) & 0x03) - 2;
				px.rgba.g += ((b1 >> 2) & 0x03) - 2;
				px.rgba.b += ( b1       & 0x03) - 2;
			}
			else if ((b1 & QOI_MASK_2) == QOI_OP_LUMA) {
				int b2 = bytes[p++];
				int vg = (b1 & 0x3f) - 32;
				px.rgba.r += vg - 8 + ((b2 >> 4) & 0x0f);
				px.rgba.g += vg;
				px.rgba.b += vg - 8 +  (b2       & 0x0f);
			}
			else if ((b1 & QOI_MASK_2) == QOI_OP_RUN) {
				run = (b1 & 0x3f);
			}

			index[QOI_COLOR_HASH(px) & (64 - 1)] = px;
		}

		pixels[px_pos + 0] = px.rgba.r;
		pixels[px_pos + 1] = px.rgba.g;
		pixels[px_pos + 2] = px.rgba.b;
		
		if (channels == 4) {
			pixels[px_pos + 3] = px.rgba.a;
		}
	}

	return pixels;
}

/* -----------------------------------------------------------------------------
Encode result cache implementation.

Key design points:
 - The key is (hash of pixels+header, width, height, channels, colorspace).
   The full pixel buffer is kept alongside each entry and compared on every
   candidate match, so a collision can never return the wrong bytes.
 - Eviction is configurable: LRU (moves hits to the front), FIFO (insertion
   order) or random.
 - Memory accounting includes entry struct + key copy of the pixels + encoded
   bytes + per-entry bookkeeping overhead, so `capacity_bytes` bounds the heap
   the cache holds (plus the fixed global state).
 - The store is a simple doubly-linked list; capacities in practice are small
   (images are hundreds of KB), so an O(n) scan per lookup is not a concern.
*/

#define QOI_CACHE_ENTRY_OVERHEAD 64u /* allocator bookkeeping, rounded up */

typedef struct qoi_cache_entry {
	unsigned long long hash;
	qoi_desc desc;
	unsigned int pixels_len;
	unsigned char *pixels;    /* copy of input, for collision verification */
	unsigned char *encoded;   /* exact bytes qoi_encode() returned */
	unsigned int encoded_len;
	struct qoi_cache_entry *next, *prev;
} qoi_cache_entry;

/* 0 = weak hash disabled. Declared before qoi_cache_weak_hash(). */
static int qoi_cache_weak_bits = 0;

static struct {
	qoi_cache_entry *head, *tail; /* head = newest, tail = eviction end */
	unsigned int entries;
	unsigned long long bytes;
	unsigned int capacity;
	int policy;
	unsigned int rand_state;
	qoi_cache_hash_fn hash;
	qoi_cache_stats stats;
} qoi_cache = {
	NULL, NULL, 0, 0, QOI_CACHE_DEFAULT_CAPACITY, QOI_CACHE_DEFAULT_POLICY,
	0x2545F491u, NULL, {0,0,0,0,0,0,0}
};

/* 64-bit FNV-1a over the header fields that affect encoding output first, then
the pixel bytes, with the byte length folded in last. The header is included so
two images with identical pixels but a different desc never share a key (the
desc is also compared explicitly; the hash just avoids pointless comparisons).
Bytes are consumed 8 at a time where alignment allows, which keeps the hash
cheap relative to encoding. */
static unsigned long long qoi_cache_default_hash(const void *data, const qoi_desc *desc) {
	unsigned long long h = 1469598103934665603ULL; /* FNV-1a 64 offset basis */
	const unsigned char *p = (const unsigned char *)data;
	const unsigned char hdr[10] = {
		(unsigned char)(desc->width >> 24), (unsigned char)(desc->width >> 16),
		(unsigned char)(desc->width >>  8), (unsigned char)(desc->width),
		(unsigned char)(desc->height >> 24), (unsigned char)(desc->height >> 16),
		(unsigned char)(desc->height >>  8), (unsigned char)(desc->height),
		desc->channels, desc->colorspace
	};
	unsigned int i, n = desc->width * desc->height * desc->channels;

	for (i = 0; i < sizeof(hdr); i++) {
		h ^= hdr[i];
		h *= 1099511628211ULL; /* FNV prime */
	}
	for (i = 0; i + 8 <= n; i += 8) {
		unsigned long long w;
		memcpy(&w, p + i, 8);
		h ^= w;
		h *= 1099511628211ULL;
		h ^= h >> 32;
	}
	for (; i < n; i++) {
		h ^= p[i];
		h *= 1099511628211ULL;
	}
	h ^= n;
	h *= 1099511628211ULL;
	return h;
}

unsigned long long qoi_cache_weak_hash(const void *data, const qoi_desc *desc) {
	/* fold the full hash down to the low `bits` bits, mirroring a weak hash */
	int bits = qoi_cache_weak_bits;
	unsigned long long h = qoi_cache_default_hash(data, desc);
	if (bits <= 0)   return 0;
	if (bits >= 64)  return h;
	return h & ((1ULL << bits) - 1);
}

static void qoi_cache_unlink(qoi_cache_entry *e) {
	if (e->prev) e->prev->next = e->next; else qoi_cache.head = e->next;
	if (e->next) e->next->prev = e->prev; else qoi_cache.tail = e->prev;
	e->next = e->prev = NULL;
}

static void qoi_cache_free_entry(qoi_cache_entry *e) {
	qoi_cache.bytes -= e->pixels_len + e->encoded_len + sizeof(*e) + QOI_CACHE_ENTRY_OVERHEAD;
	QOI_FREE(e->pixels);
	QOI_FREE(e->encoded);
	QOI_FREE(e);
}

static void qoi_cache_clear_impl(int count_evictions) {
	qoi_cache_entry *e = qoi_cache.head;
	while (e) {
		qoi_cache_entry *next = e->next;
		if (count_evictions) qoi_cache.stats.evictions++;
		qoi_cache_free_entry(e);
		e = next;
	}
	qoi_cache.head = qoi_cache.tail = NULL;
	qoi_cache.entries = 0;
	qoi_cache.bytes = 0;
	qoi_cache.stats.entries = 0;
	qoi_cache.stats.bytes = 0;
}

static qoi_cache_entry *qoi_cache_victim(void);

void qoi_cache_config(unsigned int capacity_bytes, int policy) {
	if (policy != QOI_CACHE_LRU && policy != QOI_CACHE_FIFO && policy != QOI_CACHE_RAND) {
		return;
	}
	qoi_cache.capacity = capacity_bytes;
	qoi_cache.policy = policy;
	while (qoi_cache.entries > 0 && qoi_cache.bytes > qoi_cache.capacity) {
		qoi_cache_entry *old = qoi_cache_victim();
		if (!old) break;
		qoi_cache_unlink(old);
		qoi_cache.stats.evictions++;
		qoi_cache.entries--;
		qoi_cache_free_entry(old);
	}
	qoi_cache.stats.entries = qoi_cache.entries;
	qoi_cache.stats.bytes = qoi_cache.bytes;
}

void qoi_cache_set_hash(qoi_cache_hash_fn fn) {
	/* Stored fingerprints only mean something for the hash that made them, so
	   a hash change invalidates every entry. The statistics are kept: they
	   describe calls, not the hash. */
	qoi_cache_clear_impl(0);
	qoi_cache.hash = fn ? fn : qoi_cache_default_hash;
}

qoi_cache_hash_fn qoi_cache_get_hash(void) {
	return qoi_cache.hash ? qoi_cache.hash : qoi_cache_default_hash;
}

void qoi_cache_set_weak_hash(unsigned int bits) {
	qoi_cache_weak_bits = (int)bits;
	qoi_cache_set_hash(qoi_cache_weak_hash);
}

/* Pick the entry to evict: tail is least-recently-used for LRU (hits move to
the head) and oldest-inserted for FIFO (hits never reorder). Random picks an
index with a small LCG. */
static qoi_cache_entry *qoi_cache_victim(void) {
	qoi_cache_entry *e;
	if (qoi_cache.policy == QOI_CACHE_RAND && qoi_cache.entries > 0) {
		unsigned int k = (qoi_cache.rand_state = qoi_cache.rand_state * 1103515245u + 12345u)
			% qoi_cache.entries;
		for (e = qoi_cache.head; e && k > 0; e = e->next, k--) { }
		return e ? e : qoi_cache.tail;
	}
	return qoi_cache.tail; /* LRU: tail is least recently used; FIFO: oldest */
}

static void qoi_cache_make_room(unsigned long long need) {
	while (qoi_cache.entries > 0 && qoi_cache.bytes + need > qoi_cache.capacity) {
		qoi_cache_entry *victim = qoi_cache_victim();
		if (!victim) break;
		qoi_cache_unlink(victim);
		qoi_cache.stats.evictions++;
		qoi_cache.entries--;
		qoi_cache_free_entry(victim);
	}
}

void *qoi_encode_cached(const void *data, const qoi_desc *desc, int *out_len) {
	qoi_cache_entry *e;
	unsigned int px_len;
	unsigned long long h;

	if (data == NULL || out_len == NULL || desc == NULL ||
		desc->width == 0 || desc->height == 0 ||
		desc->channels < 3 || desc->channels > 4 ||
		desc->colorspace > 1 ||
		desc->height >= QOI_PIXELS_MAX / desc->width) {
		/* Let qoi_encode() produce the canonical failure (and *out_len
		untouched). No fingerprint is computed for invalid input. */
		return qoi_encode(data, desc, out_len);
	}

	px_len = desc->width * desc->height * desc->channels;
	h = (qoi_cache.hash ? qoi_cache.hash : qoi_cache_default_hash)(data, desc);

	/* find a candidate with the same fingerprint and header */
	for (e = qoi_cache.head; e; e = e->next) {
		if (e->hash != h || e->desc.width != desc->width ||
			e->desc.height != desc->height ||
			e->desc.channels != desc->channels ||
			e->desc.colorspace != desc->colorspace) {
			continue;
		}
		/* Same key. Verify content; if it differs this is a collision. */
		if (e->pixels_len == px_len && memcmp(e->pixels, data, px_len) == 0) {
			unsigned char *copy = (unsigned char *)QOI_MALLOC(e->encoded_len);
			if (!copy) return NULL;
			memcpy(copy, e->encoded, e->encoded_len);
			*out_len = (int)e->encoded_len;
			qoi_cache.stats.hits++;
			/* LRU: move to head so it is kept longest */
			if (qoi_cache.policy == QOI_CACHE_LRU && e != qoi_cache.head) {
				qoi_cache_unlink(e);
				e->prev = NULL;
				e->next = qoi_cache.head;
				if (qoi_cache.head) qoi_cache.head->prev = e;
				qoi_cache.head = e;
				if (!qoi_cache.tail) qoi_cache.tail = e;
			}
			return copy;
		}
		qoi_cache.stats.collisions++;
	}

	/* miss: encode with the untouched original function, then store */
	{
		void *encoded = qoi_encode(data, desc, out_len);
		unsigned long long need;
		unsigned char *px_copy;
		qoi_cache_entry *n;
		if (!encoded) return NULL;

		qoi_cache.stats.misses++;

		need = (unsigned long long)px_len + (unsigned long long)*out_len +
			sizeof(qoi_cache_entry) + QOI_CACHE_ENTRY_OVERHEAD;
		if (qoi_cache.capacity == 0 || need > qoi_cache.capacity) {
			return encoded; /* can never fit */
		}
		px_copy = (unsigned char *)QOI_MALLOC(px_len);
		n = (qoi_cache_entry *)QOI_MALLOC(sizeof(qoi_cache_entry));
		if (!px_copy || !n) {
			QOI_FREE(px_copy);
			QOI_FREE(n);
			return encoded;
		}
		memcpy(px_copy, data, px_len);

		qoi_cache_make_room(need);
		if (qoi_cache.bytes + need > qoi_cache.capacity) {
			/* still doesn't fit (e.g. tiny capacity); don't store */
			QOI_FREE(px_copy);
			QOI_FREE(n);
			return encoded;
		}

		n->hash = h;
		n->desc = *desc;
		n->pixels_len = px_len;
		n->pixels = px_copy;
		n->encoded = (unsigned char *)QOI_MALLOC((unsigned int)*out_len);
		if (!n->encoded) {
			QOI_FREE(px_copy);
			QOI_FREE(n);
			return encoded;
		}
		memcpy(n->encoded, encoded, (unsigned int)*out_len);
		n->encoded_len = (unsigned int)*out_len;
		n->prev = NULL;
		n->next = qoi_cache.head;
		if (qoi_cache.head) qoi_cache.head->prev = n;
		qoi_cache.head = n;
		if (!qoi_cache.tail) qoi_cache.tail = n;
		qoi_cache.entries++;
		qoi_cache.bytes += need;
		qoi_cache.stats.stores++;
		qoi_cache.stats.entries = qoi_cache.entries;
		qoi_cache.stats.bytes = qoi_cache.bytes;

		return encoded;
	}
}

void qoi_cache_get_stats(qoi_cache_stats *out) {
	if (out) {
		*out = qoi_cache.stats;
		out->entries = qoi_cache.entries;
		out->bytes = qoi_cache.bytes;
	}
}

void qoi_cache_clear(void) {
	qoi_cache_clear_impl(0);
	/* clearing starts a fresh measurement window */
	memset(&qoi_cache.stats, 0, sizeof(qoi_cache.stats));
}

void qoi_cache_destroy(void) {
	qoi_cache_clear_impl(0);
	qoi_cache.capacity = QOI_CACHE_DEFAULT_CAPACITY;
	qoi_cache.policy = QOI_CACHE_DEFAULT_POLICY;
	qoi_cache.hash = NULL;
	qoi_cache_weak_bits = 0;
	memset(&qoi_cache.stats, 0, sizeof(qoi_cache.stats));
}

#ifndef QOI_NO_STDIO
#include <stdio.h>

int qoi_write(const char *filename, const void *data, const qoi_desc *desc) {
	FILE *f = fopen(filename, "wb");
	int size, err;
	void *encoded;

	if (!f) {
		return 0;
	}

	encoded = qoi_encode(data, desc, &size);
	if (!encoded) {
		fclose(f);
		return 0;
	}

	fwrite(encoded, 1, size, f);
	fflush(f);
	err = ferror(f);
	fclose(f);

	QOI_FREE(encoded);
	return err ? 0 : size;
}

void *qoi_read(const char *filename, qoi_desc *desc, int channels) {
	FILE *f = fopen(filename, "rb");
	int size, bytes_read;
	void *pixels, *data;

	if (!f) {
		return NULL;
	}

	fseek(f, 0, SEEK_END);
	size = ftell(f);
	if (size <= 0 || fseek(f, 0, SEEK_SET) != 0) {
		fclose(f);
		return NULL;
	}

	data = QOI_MALLOC(size);
	if (!data) {
		fclose(f);
		return NULL;
	}

	bytes_read = fread(data, 1, size, f);
	fclose(f);
	pixels = (bytes_read != size) ? NULL : qoi_decode(data, bytes_read, desc, channels);
	QOI_FREE(data);
	return pixels;
}

#endif /* QOI_NO_STDIO */
#endif /* QOI_IMPLEMENTATION */
