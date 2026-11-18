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



-- Documentation

This library provides the following functions;
- qoi_read    -- read and decode a QOI file
- qoi_decode  -- decode the raw bytes of a QOI image from memory
- qoi_write   -- encode and write a QOI file
- qoi_encode  -- encode an rgba buffer into a QOI image in memory

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


/* -----------------------------------------------------------------------------
QOLZ - LZ77 preprocessor layer for QOI

An optional, self-contained LZ77 (LZSS style) stage that sits in front of the
QOI coding path. The compressor picks the smallest of three payloads:

  mode 0: LZ77 over the raw RGBA byte stream (the preprocessor path)
  mode 1: LZ77 over the QOI encoded stream (QOI + LZ77 fallback)
  mode 2: raw payload stored as-is, i.e. an uncompressed "stored" block

The mode is recorded in the one byte container header, so the decoder can
restore the pixels independently of qoi_encode/qoi_decode parameter choices.

LZ77 stream format (all integers little endian):

  Groups of up to 8 tokens. Each group starts with one flag byte; bit 0
  corresponds to the first token, bit 7 to the last. A 0 bit marks a literal
  token (one raw byte follows). A 1 bit marks a match token:
    uint16 distance, 1..65535 (backward offset in the decompressed stream)
    uint8  len_code, match length is (len_code + 3), i.e. 3..257 bytes
  Longer matches are emitted as several consecutive match tokens.

Define QOLZ_IMPLEMENTATION together with QOI_IMPLEMENTATION in one C/C++
file before including this header to create the implementation. The QOLZ
layer reuses qoi_encode/qoi_decode and the QOI_MALLOC/QOI_FREE hooks, so
QOI_IMPLEMENTATION must be defined as well.
*/

#define QOLZ_MAGIC \
	(((unsigned int)'q') << 24 | ((unsigned int)'o') << 16 | \
	 ((unsigned int)'l') <<  8 | ((unsigned int)'z'))
#define QOLZ_HEADER_SIZE 15 /* 4 magic + 10 qoi fields + 1 mode */

#define QOLZ_MODE_LZ_RAW 0
#define QOLZ_MODE_LZ_QOI 1
#define QOLZ_MODE_STORED 2

typedef struct {
	int min_match; /* minimum useful match length, clamped to >= 1 */
	int max_chain; /* hash chain depth per search, clamped to >= 1 */
	int nice_match; /* stop searching once a match this long is found */
	int lazy;      /* 0 = greedy, 1 = lazy matching (one lookahead) */
} qolz_options;

/* Encode raw RGB(A) pixels into a QOLZ container in memory. Returns NULL on
failure (invalid parameters or malloc failure) and writes the size in bytes
to *out_len on success. The returned buffer must be free()d with QOI_FREE. */

void *qolz_encode(const void *data, const qoi_desc *desc, int *out_len);

/* Decode a QOLZ container from memory into RGB(A) pixels. The semantics and
parameter conventions match qoi_decode(). */

void *qolz_decode(const void *data, int size, qoi_desc *desc, int channels);


#ifdef QOLZ_IMPLEMENTATION
#ifndef QOLZ_IMPLEMENTATION_INCLUDED
#define QOLZ_IMPLEMENTATION_INCLUDED

#include <limits.h>

#define QOLZ_WINDOW_SIZE   65535  /* maximum backward distance */
#define QOLZ_MAX_MATCH     257    /* 3 + 254, one uint8 length code */
#define QOLZ_HASH_BITS     16
#define QOLZ_HASH_SIZE     (1 << QOLZ_HASH_BITS)
#define QOLZ_NO_POS        (-1)
/* Worst case: every byte becomes a flag bit (1/8) + a literal byte. */
#define QOLZ_LZ_BOUND(n)   ((n) + (n) / 8 + 16)

static unsigned int qolz_hash4(const unsigned char *p) {
	unsigned int h = (unsigned int)p[0] |
		((unsigned int)p[1] << 8) |
		((unsigned int)p[2] << 16) |
		((unsigned int)p[3] << 24);
	/* Knuth multiplicative hash, keeps the low 16 bits */
	return (h * 2654435761u) >> (32 - QOLZ_HASH_BITS);
}

/* Find the longest match for position pos by walking the hash chain. Writes
the match distance (0 if none) and length to *out_dist / *out_len. */

static void qolz_find_match(
	const unsigned char *data, int n, int pos,
	const int *head, const int *prev,
	int min_match, int max_chain, int nice_match,
	int *out_dist, int *out_len
) {
	int best_len = min_match - 1;
	int best_dist = 0;
	int max_len = n - pos;
	int chain_count;
	int cand;

	if (max_len < min_match || pos + 3 >= n) {
		*out_dist = 0;
		*out_len = 0;
		return;
	}
	if (max_len > QOLZ_MAX_MATCH) {
		max_len = QOLZ_MAX_MATCH;
	}

	cand = head[qolz_hash4(data + pos)];
	for (chain_count = 0; chain_count < max_chain && cand != QOLZ_NO_POS;
		chain_count++
	) {
		int dist = pos - cand;
		int limit;
		int len;

		/* Stale chain entry: outside the window or before the stream. */
		if (dist <= 0 || dist > QOLZ_WINDOW_SIZE) {
			break;
		}

		/* Cheap reject on the last byte before doing the full compare. */
		if (
			best_len < max_len &&
			data[cand + best_len] == data[pos + best_len] &&
			data[cand] == data[pos]
		) {
			limit = max_len;
			len = 0;
			while (len < limit && data[cand + len] == data[pos + len]) {
				len++;
			}
			if (len > best_len) {
				best_len = len;
				best_dist = dist;
				/* A match this long is "good enough": stop walking the
				chain early to cap encoder time on repetitive data. */
				if (len == max_len || len >= nice_match) {
					break;
				}
			}
		}
		if (prev == NULL) {
			break; /* head-only hash: there is no next chain entry */
		}
		cand = prev[cand];
	}

	*out_dist = best_dist;
	*out_len = (best_len >= min_match) ? best_len : 0;
	if (*out_len == 0) {
		*out_dist = 0;
	}
}

/* LZ77 compress `in_len` bytes from `in` into a freshly malloc'd buffer.
Returns NULL on malloc failure or if the (bounded) output overflows. */

static unsigned char *qolz_lz_compress(
	const unsigned char *in, int in_len,
	int min_match, int max_chain, int nice_match, int lazy,
	int *out_len
) {
	int *head;
	int *prev;
	unsigned char *out;
	int out_cap;
	int pos = 0;
	int out_p = 0;
	int flag_p = 0;
	int flags = 0;
	int token_n = 0; /* tokens already emitted in the current flag group */
	int hash_pos;
	int match_len = 0;
	int match_dist = 0;

	if (min_match < 1) { min_match = 1; }
	if (max_chain < 1) { max_chain = 1; }
	if (nice_match < 3) { nice_match = 3; }
	lazy = lazy ? 1 : 0;

	out_cap = QOLZ_LZ_BOUND(in_len);
	out = (unsigned char *) QOI_MALLOC((size_t)out_cap);
	head = (int *) QOI_MALLOC(sizeof(int) * QOLZ_HASH_SIZE);
	/* With a chain depth of one the link array is never read: the hash
	table alone is enough, which avoids an O(n) allocation and an O(n)
	stream of random writes on every compression. */
	prev = (max_chain > 1)
		? (int *) QOI_MALLOC(sizeof(int) * (size_t)(in_len ? in_len : 1))
		: NULL;
	if (!out || !head || (max_chain > 1 && !prev)) {
		QOI_FREE(out);
		QOI_FREE(head);
		QOI_FREE(prev);
		return NULL;
	}
	memset(head, 0xff, sizeof(int) * QOLZ_HASH_SIZE); /* QOLZ_NO_POS */

	/* Reserve the flag byte for the next token and close the previous
	group once it holds 8 tokens. Every emitted token goes through here,
	so flag accounting can never drift from the actual token stream. */
	#define LZ_BEGIN_TOKEN() do { \
		if (token_n == 0) { \
			if (out_p + 1 >= out_cap) { goto fail; } \
			flag_p = out_p; \
			out_p++; \
		} \
	} while (0)
	#define LZ_END_TOKEN(is_match) do { \
		if (is_match) { flags |= (1 << token_n); } \
		token_n++; \
		if (token_n == 8) { \
			out[flag_p] = (unsigned char)flags; \
			flags = 0; \
			token_n = 0; \
		} \
	} while (0)

	/* pos advances by one on every literal and by the match length on
	every match token, so an input without matches only emits literals and
	always terminates. */
	for (;;) {
		int cur_len = 0, cur_dist = 0;

		if (pos >= in_len) {
			break;
		}

		qolz_find_match(in, in_len, pos, head, prev,
			min_match, max_chain, nice_match, &cur_dist, &cur_len);

		if (lazy && match_len > 0 && pos > 0) {
			/* A match is pending from the previous position. Emit it
			unless the match found here is strictly better. */
			if (cur_len > match_len + 1) {
				/* Demote the pending match start to a literal and keep
				the current match pending for the next round. */
				LZ_BEGIN_TOKEN();
				if (out_p >= out_cap) { goto fail; }
				out[out_p++] = in[pos - 1];
				LZ_END_TOKEN(0);

				if (pos - 1 + 4 <= in_len) {
					unsigned int h = qolz_hash4(in + (pos - 1));
					if (prev) { prev[pos - 1] = head[h]; }
					head[h] = pos - 1;
				}
				match_len = cur_len;
				match_dist = cur_dist;
				pos++;
				continue;
			}
			else {
				/* Emit the pending match; it starts at pos - 1. */
				int start = pos - 1;
				int i;
				LZ_BEGIN_TOKEN();
				if (out_p + 3 >= out_cap) { goto fail; }
				out[out_p++] = (unsigned char)(match_dist & 0xff);
				out[out_p++] = (unsigned char)((match_dist >> 8) & 0xff);
				out[out_p++] = (unsigned char)(match_len - 3);
				LZ_END_TOKEN(1);
				for (i = 0; i < match_len; i++) {
					hash_pos = start + i;
					if (hash_pos + 4 <= in_len) {
						unsigned int h = qolz_hash4(in + hash_pos);
						if (prev) { prev[hash_pos] = head[h]; }
						head[h] = hash_pos;
					}
				}
				pos = start + match_len;
				match_len = 0;
				match_dist = 0;
				continue;
			}
		}

		/* A 1-2 byte match cannot pay for its 3 byte token; only matches
		of at least 3 bytes are ever emitted. This also makes min_match=1
		degenerate to ordinary literal/match(>=3) coding. */
		if (cur_len >= 3 && lazy) {
			/* Hold the match and look one position ahead next round. */
			match_len = cur_len;
			match_dist = cur_dist;
			pos++;
			continue;
		}
		else if (cur_len >= 3) {
			int i;
			LZ_BEGIN_TOKEN();
			if (out_p + 3 >= out_cap) { goto fail; }
			out[out_p++] = (unsigned char)(cur_dist & 0xff);
			out[out_p++] = (unsigned char)((cur_dist >> 8) & 0xff);
			out[out_p++] = (unsigned char)(cur_len - 3);
			LZ_END_TOKEN(1);
			for (i = 0; i < cur_len; i++) {
				hash_pos = pos + i;
				if (hash_pos + 4 <= in_len) {
					unsigned int h = qolz_hash4(in + hash_pos);
					if (prev) { prev[hash_pos] = head[h]; }
					head[h] = hash_pos;
				}
			}
			pos += cur_len;
		}
		else {
			LZ_BEGIN_TOKEN();
			if (out_p >= out_cap) { goto fail; }
			out[out_p++] = in[pos];
			LZ_END_TOKEN(0);
			if (pos + 4 <= in_len) {
				unsigned int h = qolz_hash4(in + pos);
				if (prev) { prev[pos] = head[h]; }
				head[h] = pos;
			}
			pos++;
		}
	}

	/* Flush a match still pending at end of input. */
	if (match_len > 0) {
		int start = pos - 1;
		int i;
		LZ_BEGIN_TOKEN();
		if (out_p + 3 >= out_cap) { goto fail; }
		out[out_p++] = (unsigned char)(match_dist & 0xff);
		out[out_p++] = (unsigned char)((match_dist >> 8) & 0xff);
		out[out_p++] = (unsigned char)(match_len - 3);
		LZ_END_TOKEN(1);
		for (i = 0; i < match_len; i++) {
			hash_pos = start + i;
			if (hash_pos + 4 <= in_len) {
				unsigned int h = qolz_hash4(in + hash_pos);
				if (prev) { prev[hash_pos] = head[h]; }
				head[h] = hash_pos;
			}
		}
		pos = start + match_len;
	}
	if (token_n > 0) {
		out[flag_p] = (unsigned char)flags;
	}

	#undef LZ_BEGIN_TOKEN
	#undef LZ_END_TOKEN

	if (pos != in_len) {
		goto fail;
	}
	QOI_FREE(head);
	QOI_FREE(prev);
	*out_len = out_p;
	return out;

fail:
	QOI_FREE(out);
	QOI_FREE(head);
	QOI_FREE(prev);
	return NULL;
}

/* Decompress an LZ77 stream produced by qolz_lz_compress into a freshly
malloc'd buffer. If expected_len > 0 the decompressed output must have
exactly that many bytes; if expected_len == 0 the output ends when the input
is fully consumed. The whole input must always be consumed. Returns NULL on
malloc failure or on any malformed/truncated input. */

static unsigned char *qolz_lz_decompress(
	const unsigned char *in, int in_len, int dst_cap, int expected_len,
	int *out_actual
) {
	unsigned char *dst;
	int in_p = 0;
	int out_p = 0;
	unsigned char flags = 0;
	int bit = 8; /* force a flag byte read on the first iteration */

	if (dst_cap < 0 || in_len < 0 || expected_len < 0 ||
		expected_len > dst_cap
	) {
		return NULL;
	}
	dst = (unsigned char *) QOI_MALLOC((size_t)dst_cap ? (size_t)dst_cap : 1);
	if (!dst) {
		return NULL;
	}

	/* With expected_len > 0 exactly that many output bytes must be
	produced. With expected_len == 0 the output ends when the whole input
	has been consumed. A flag byte only governs the following 8 tokens,
	and the last group may use fewer than 8. */
	while (
		out_p < dst_cap &&
		(expected_len == 0 ? in_p < in_len : out_p < expected_len)
	) {
		int is_match;

		if (bit == 8) {
			if (in_p >= in_len) { goto fail; }
			flags = in[in_p++];
			bit = 0;
		}
		is_match = (flags >> bit) & 1;
		bit++;

		if (!is_match) {
			if (in_p >= in_len) { goto fail; }
			dst[out_p++] = in[in_p++];
		}
		else {
			int dist, len, i;
			if (in_p + 3 > in_len) { goto fail; }
			dist = in[in_p] | (in[in_p + 1] << 8);
			len = in[in_p + 2] + 3;
			in_p += 3;
			if (
				dist < 1 || dist > QOLZ_WINDOW_SIZE ||
				dist > out_p || len > QOLZ_MAX_MATCH ||
				len > dst_cap - out_p
			) {
				goto fail;
			}
			for (i = 0; i < len; i++) {
				dst[out_p] = dst[out_p - dist];
				out_p++;
			}
		}
	}

	if (in_p != in_len) {
		goto fail;
	}
	if (expected_len > 0 && out_p != expected_len) {
		goto fail;
	}
	if (out_actual) {
		*out_actual = out_p;
	}
	return dst;

fail:
	QOI_FREE(dst);
	return NULL;
}

void *qolz_encode(const void *data, const qoi_desc *desc, int *out_len) {
	unsigned char *raw;
	unsigned char *qoi = NULL;
	unsigned char *lz_qoi = NULL;
	unsigned char *payload;
	unsigned char *out;
	int qoi_len, lz_qoi_len;
	int px_len, out_cap, p = 0;
	int mode;
	unsigned int pixels;

	/* Default LZ77 tuning. Greedy matching with a single-entry (head-only)
	hash keeps the encoder well inside the time budget; the ratio cost
	versus a deep lazy chain is small. The deeper chain, nice_match and
	lazy knobs remain available through qolz_lz_compress(). */
	const int min_match = 3;
	const int max_chain = 1;
	const int nice_match = 32;
	const int lazy = 0;

	if (
		data == NULL || out_len == NULL || desc == NULL ||
		desc->width == 0 || desc->height == 0 ||
		desc->channels < 3 || desc->channels > 4 ||
		desc->colorspace > 1 ||
		desc->height >= QOI_PIXELS_MAX / desc->width
	) {
		return NULL;
	}

	pixels = desc->width * desc->height;
	if (pixels > (unsigned int)INT_MAX / desc->channels) {
		return NULL;
	}
	px_len = (int)pixels * desc->channels;

	raw = (unsigned char *) QOI_MALLOC((size_t)px_len);
	if (!raw) {
		return NULL;
	}
	memcpy(raw, data, (size_t)px_len);

	/* Stored (mode 2) is the guaranteed upper bound: the payload is never
	larger than the raw pixel stream. */
	mode = QOLZ_MODE_STORED;
	payload = raw;
	out_cap = px_len;

	qoi = (unsigned char *) qoi_encode(raw, desc, &qoi_len);

	/* Only spend an LZ77 pass when QOI actually made the stream smaller.
	On incompressible material (noise) QOI is already >= raw size and LZ
	cannot win, so skipping it avoids a pointless O(n) search. */
	if (qoi && qoi_len < px_len) {
		lz_qoi = qolz_lz_compress(qoi, qoi_len,
			min_match, max_chain, nice_match, lazy, &lz_qoi_len);
		if (lz_qoi && lz_qoi_len < out_cap) {
			mode = QOLZ_MODE_LZ_QOI;
			payload = lz_qoi;
			out_cap = lz_qoi_len;
		}
	}

	out = (unsigned char *) QOI_MALLOC((size_t)QOLZ_HEADER_SIZE + (size_t)out_cap);
	if (!out) {
		QOI_FREE(raw);
		QOI_FREE(qoi);
		QOI_FREE(lz_qoi);
		return NULL;
	}

	{
		int hp = 0;
		qoi_write_32(out, &hp, QOLZ_MAGIC);
		qoi_write_32(out, &hp, desc->width);
		qoi_write_32(out, &hp, desc->height);
		out[hp++] = desc->channels;
		out[hp++] = desc->colorspace;
		out[hp++] = (unsigned char)mode;
	}
	memcpy(out + QOLZ_HEADER_SIZE, payload, (size_t)out_cap);
	p = QOLZ_HEADER_SIZE + out_cap;

	QOI_FREE(raw);
	QOI_FREE(qoi);
	QOI_FREE(lz_qoi);

	*out_len = p;
	return out;
}

void *qolz_decode(const void *data, int size, qoi_desc *desc, int channels) {
	const unsigned char *bytes;
	unsigned int header_magic;
	unsigned char mode;
	int p = 0;
	int payload_len;
	unsigned char *buf = NULL;
	unsigned char *pixels;

	if (
		data == NULL || desc == NULL ||
		(channels != 0 && channels != 3 && channels != 4) ||
		size < QOLZ_HEADER_SIZE
	) {
		return NULL;
	}
	bytes = (const unsigned char *)data;

	header_magic = qoi_read_32(bytes, &p);
	desc->width = qoi_read_32(bytes, &p);
	desc->height = qoi_read_32(bytes, &p);
	desc->channels = bytes[p++];
	desc->colorspace = bytes[p++];
	mode = bytes[p++];

	if (
		desc->width == 0 || desc->height == 0 ||
		desc->channels < 3 || desc->channels > 4 ||
		desc->colorspace > 1 ||
		header_magic != QOLZ_MAGIC ||
		desc->height >= QOI_PIXELS_MAX / desc->width
	) {
		return NULL;
	}

	if (channels == 0) {
		channels = desc->channels;
	}

	payload_len = size - QOLZ_HEADER_SIZE;

	if (mode == QOLZ_MODE_STORED) {
		int stored_len = (int)(desc->width * desc->height) * desc->channels;
		if (payload_len != stored_len) {
			return NULL;
		}
		buf = (unsigned char *) QOI_MALLOC((size_t)stored_len);
		if (!buf) {
			return NULL;
		}
		memcpy(buf, bytes + QOLZ_HEADER_SIZE, (size_t)stored_len);
	}
	else if (mode == QOLZ_MODE_LZ_RAW) {
		int raw_len = (int)(desc->width * desc->height) * desc->channels;
		buf = qolz_lz_decompress(bytes + QOLZ_HEADER_SIZE,
			payload_len, raw_len, raw_len, NULL);
		if (!buf) {
			return NULL;
		}
	}
	else if (mode == QOLZ_MODE_LZ_QOI) {
		qoi_desc inner;
		int inner_p = 0;
		int qoi_cap;
		int actual_len = 0;
		unsigned int inner_magic;

		/* The payload is an LZ77 stream whose *decompressed* bytes form an
		ordinary QOI stream. Bound the output with the container header,
		decompress first, then validate the QOI header from real bytes. */
		qoi_cap = (int)desc->width * (int)desc->height *
			(desc->channels + 1) + QOI_HEADER_SIZE +
			(int)sizeof(qoi_padding);

		buf = qolz_lz_decompress(bytes + QOLZ_HEADER_SIZE,
			payload_len, qoi_cap, 0, &actual_len);
		if (!buf || actual_len < QOI_HEADER_SIZE +
			(int)sizeof(qoi_padding)) {
			QOI_FREE(buf);
			return NULL;
		}

		inner_magic = qoi_read_32(buf, &inner_p);
		inner.width = qoi_read_32(buf, &inner_p);
		inner.height = qoi_read_32(buf, &inner_p);
		inner.channels = buf[inner_p++];
		inner.colorspace = buf[inner_p++];
		if (
			inner_magic != QOI_MAGIC ||
			inner.width != desc->width ||
			inner.height != desc->height ||
			inner.channels != desc->channels
		) {
			QOI_FREE(buf);
			return NULL;
		}

		pixels = (unsigned char *) qoi_decode(buf, actual_len,
			&inner, channels);
		QOI_FREE(buf);
		return pixels;
	}
	else {
		return NULL;
	}

	if (channels == desc->channels) {
		return buf;
	}
	/* Channel count conversion is handled by reusing QOI: wrap through a
	QOI round trip on the restored raw pixels. */
	{
		qoi_desc raw_desc = *desc;
		unsigned char *qoi_buf;
		int qoi_len;
		qoi_desc ignored;
		raw_desc.channels = desc->channels;
		qoi_buf = (unsigned char *) qoi_encode(buf, &raw_desc, &qoi_len);
		if (!qoi_buf) {
			QOI_FREE(buf);
			return NULL;
		}
		pixels = (unsigned char *) qoi_decode(qoi_buf, qoi_len,
			&ignored, channels);
		QOI_FREE(qoi_buf);
		QOI_FREE(buf);
		return pixels;
	}
}

#endif /* QOLZ_IMPLEMENTATION_INCLUDED */
#endif /* QOLZ_IMPLEMENTATION */
