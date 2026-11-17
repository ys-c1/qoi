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


/* -----------------------------------------------------------------------------
QBA - Block-adaptive extension (experimental, lossless)

QBA wraps an image into independently coded horizontal bands of
`block_height` rows. Each block is analyzed for complexity (number of
distinct colors, longest pixel run) and one of three codings is selected per
block: raw copy, run-length or palette index. A per-block header records the
mode, payload length and a CRC-8 checksum, so a corrupted block header can be
detected; the decoder then resynchronizes on the next block marker, skips
the damaged block and continues with the remaining blocks.

The original qoi_* functions and the QOI byte stream are not affected. */

#define QBA_MAGIC      0x71626131u /* "qba1" */
#define QBA_VERSION    1
#define QBA_END_MARKER 0x51424145u /* "QBAE" */

#define QBA_MODE_RAW   0
#define QBA_MODE_RLE   1
#define QBA_MODE_INDEX 2

#define QBA_BLOCK_HEADER_SIZE 10  /* 'Q','B',mode,blocknum,len(4 BE),crc8 */

/* Configuration for block-adaptive coding. Pass NULL to qba_encode() for the
defaults. block_height is the band height in rows (>= 1). The decision uses
two thresholds against block statistics:
  maxrun - longest run of identical pixels
  ndist  - number of distinct colors in the block
    maxrun >= rle_threshold              -> QBA_MODE_RLE
    ndist  <= index_colors_threshold     -> QBA_MODE_INDEX
    otherwise                            -> QBA_MODE_RAW
index_colors_threshold must be in 1..255 (palette indices are one byte). */

typedef struct {
	unsigned int block_height;            /* rows per block, default 16    */
	unsigned int rle_threshold;           /* default 4                     */
	unsigned int index_colors_threshold;  /* default 32, range 1..255      */
} qba_config;

/* Per-image statistics, filled by qba_decode() when a non-NULL pointer is
supplied. mode_counts[m] counts blocks coded with mode m, corrupted_blocks
counts block headers that failed the CRC-8 / sanity checks and were skipped,
and header_bytes is the total on-disk size spent on block headers. */

typedef struct {
	unsigned int total_blocks;
	unsigned int mode_counts[3];
	unsigned int corrupted_blocks;
	unsigned int header_bytes;
} qba_stats;

/* Fill *cfg with the library defaults. */

void qba_default_config(qba_config *cfg);

/* Encode raw RGB/RGBA pixels into a QBA stream in memory. Returns NULL on
failure; *out_len receives the stream size. The returned buffer must be
free()d. */

void *qba_encode(const void *data, const qoi_desc *desc,
	const qba_config *cfg, int *out_len);

/* Decode a QBA stream. Pixels of skipped (corrupt header) blocks are filled
with the sentinel color (magenta, {255,0,255,255}); the function still
returns the full pixel buffer. Returns NULL only on hard failure (bad
parameters, bad container header, malloc failure). If stats is non-NULL it
is zeroed and filled with per-image coding statistics. */

void *qba_decode(const void *data, int size, qoi_desc *desc, int channels,
	qba_stats *stats);


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


/* -----------------------------------------------------------------------------
QBA implementation - block-adaptive coding

Container layout (all multi-byte integers big-endian):

  container header, 24 bytes:
    0..3   magic "qba1"
    4      version (1)
    5      channels (3 or 4)
    6      colorspace (0 or 1, informative)
    7..10  width
    11..14 height
    15..18 block_height (rows per block)
    19..20 rle_threshold (u16)
    21..22 index_colors_threshold (u16)
    23     reserved (0)

  any number of block headers + payloads:
    block header, 10 bytes:
      0..1   marker 'Q','B'
      2      mode (0 = raw, 1 = rle, 2 = index)
      3..4   block number (u16, starting at 0)
      5..8   payload length in bytes (u32)
      9      crc-8 (poly 0x07, init 0) over bytes 0..8

    raw payload:   width*blockrows*channels raw pixel bytes
    rle payload:   packbits-style control stream over whole pixels
                     control c >= 0: c+1 literal pixels follow
                     control c <  0: 1-c copies of the one pixel that follows
    index payload: 1 byte color count minus 1, then the palette (count *
                   channels bytes), then one palette index byte per pixel

  end marker, 8 bytes: "QBAE" followed by 4 zero bytes
*/

#define QBA_CONTAINER_HEADER_SIZE 24
#define QBA_CONTAINER_END_SIZE    8
#define QBA_BLOCKS_MAX            65535u

static unsigned int qba_pixel_key(const unsigned char *px, int channels) {
	if (channels == 4) {
		return (unsigned int)px[0] | ((unsigned int)px[1] << 8) |
			((unsigned int)px[2] << 16) | ((unsigned int)px[3] << 24);
	}
	return (unsigned int)px[0] | ((unsigned int)px[1] << 8) |
		((unsigned int)px[2] << 16);
}

static unsigned char qba_crc8(const unsigned char *p, int n) {
	static const unsigned char poly = 0x07;
	unsigned char crc = 0;
	int i, j;
	for (i = 0; i < n; i++) {
		crc = (unsigned char)(crc ^ p[i]);
		for (j = 0; j < 8; j++) {
			if (crc & 0x80) {
				crc = (unsigned char)((crc << 1) ^ poly);
			}
			else {
				crc = (unsigned char)(crc << 1);
			}
		}
	}
	return crc;
}

static void qba_write_32(unsigned char *b, unsigned int v) {
	b[0] = (unsigned char)((v >> 24) & 0xff);
	b[1] = (unsigned char)((v >> 16) & 0xff);
	b[2] = (unsigned char)((v >> 8) & 0xff);
	b[3] = (unsigned char)(v & 0xff);
}

static unsigned int qba_read_32(const unsigned char *b) {
	return ((unsigned int)b[0] << 24) | ((unsigned int)b[1] << 16) |
		((unsigned int)b[2] << 8) | (unsigned int)b[3];
}

/* Count distinct colors (capped at 256: anything beyond cannot use palette
indexing) and the longest run of identical pixels. */

static void qba_analyze_block(const unsigned char *px, int channels, int npix,
	unsigned int *out_ndist, unsigned int *out_maxrun) {
	unsigned int seen[256];
	unsigned int ndist = 0;
	unsigned int maxrun = 0;
	unsigned int run = 0;
	unsigned int prev_key = 0;
	int i;

	for (i = 0; i < npix; i++) {
		unsigned int key = qba_pixel_key(px + (size_t)i * channels, channels);
		unsigned int j;
		int found = 0;

		if (i == 0 || key != prev_key) {
			if (run > maxrun) { maxrun = run; }
			run = 1;
			prev_key = key;
		}
		else {
			run++;
		}

		if (ndist < 256) {
			for (j = 0; j < ndist; j++) {
				if (seen[j] == key) { found = 1; break; }
			}
			if (!found) {
				seen[ndist] = key;
				ndist++;
			}
		}
	}
	if (run > maxrun) { maxrun = run; }

	*out_ndist = ndist;
	*out_maxrun = maxrun;
}

/* Packbits-style run/literal coding over whole pixels. Returns payload size. */

static int qba_rle_encode(const unsigned char *src, int npix, int channels,
	unsigned char *out) {
	int p = 0;
	int o = 0;

	while (p < npix) {
		int run = 1;
		while (
			p + run < npix && run < 128 &&
			memcmp(src + (size_t)(p + run) * channels,
				src + (size_t)p * channels, channels) == 0
		) {
			run++;
		}

		if (run >= 2) {
			out[o++] = (unsigned char)(1 - run); /* -(run-1) */
			memcpy(out + o, src + (size_t)p * channels, channels);
			o += channels;
			p += run;
		}
		else {
			int start = p;
			int lit = 0;
			while (p < npix && lit < 128) {
				if (
					p + 1 < npix &&
					memcmp(src + (size_t)p * channels,
						src + (size_t)(p + 1) * channels, channels) == 0
				) {
					break;
				}
				p++;
				lit++;
			}
			if (lit == 0) { /* safety valve, normally unreachable */
				out[o++] = 0;
				memcpy(out + o, src + (size_t)p * channels, channels);
				o += channels;
				p++;
			}
			else {
				out[o++] = (unsigned char)(lit - 1);
				memcpy(out + o, src + (size_t)start * channels,
					(size_t)lit * channels);
				o += lit * channels;
			}
		}
	}
	return o;
}

static int qba_rle_decode(const unsigned char *in, int in_len, int channels,
	int npix, unsigned char *out) {
	int p = 0;
	int o = 0;

	while (o < npix) {
		int c, n, i;
		if (p >= in_len) { return -1; }
		c = (signed char)in[p++];

		if (c >= 0) {
			n = c + 1;
			if (o + n > npix || p + n * channels > in_len) { return -1; }
			memcpy(out + (size_t)o * channels, in + p,
				(size_t)n * channels);
			p += n * channels;
			o += n;
		}
		else {
			n = 1 - c;
			if (o + n > npix || p + channels > in_len) { return -1; }
			for (i = 0; i < n; i++) {
				memcpy(out + (size_t)o * channels, in + p, channels);
				o++;
			}
			p += channels;
		}
	}
	return (p == in_len) ? 0 : -1;
}

/* Palette coding. Returns payload size, 0 if the block has > 255 colors. */

static int qba_index_encode(const unsigned char *src, int npix, int channels,
	unsigned int ndist, unsigned char *out) {
	unsigned int palette[256];
	int ncolors = 0;
	int o = 0;
	int i, k;

	if (ndist > 255) { return 0; }
	out[o++] = (unsigned char)(ndist - 1);

	for (i = 0; i < npix; i++) {
		unsigned int key = qba_pixel_key(src + (size_t)i * channels, channels);
		int found = -1;
		for (k = 0; k < ncolors; k++) {
			if (palette[k] == key) { found = k; break; }
		}
		if (found < 0) {
			if (ncolors == 256) { return 0; }
			palette[ncolors++] = key;
		}
	}
	if ((unsigned int)ncolors != ndist) { return 0; }

	for (k = 0; k < ncolors; k++) {
		out[o++] = (unsigned char)(palette[k] & 0xff);
		out[o++] = (unsigned char)((palette[k] >> 8) & 0xff);
		out[o++] = (unsigned char)((palette[k] >> 16) & 0xff);
		if (channels == 4) {
			out[o++] = (unsigned char)((palette[k] >> 24) & 0xff);
		}
	}

	for (i = 0; i < npix; i++) {
		unsigned int key = qba_pixel_key(src + (size_t)i * channels, channels);
		for (k = 0; k < ncolors; k++) {
			if (palette[k] == key) {
				out[o++] = (unsigned char)k;
				break;
			}
		}
	}
	return o;
}

static int qba_index_decode(const unsigned char *in, int in_len, int channels,
	int npix, unsigned char *out) {
	int ncolors, pal_bytes, i;
	const unsigned char *palette;
	const unsigned char *indices;

	if (in_len < 1) { return -1; }
	ncolors = in[0] + 1;
	pal_bytes = ncolors * channels;
	if (pal_bytes <= 0 || 1 + pal_bytes + npix != in_len) { return -1; }

	palette = in + 1;
	indices = in + 1 + pal_bytes;

	for (i = 0; i < npix; i++) {
		int k = indices[i];
		if (k >= ncolors) { return -1; }
		memcpy(out + (size_t)i * channels,
			palette + (size_t)k * channels, channels);
	}
	return 0;
}

static void qba_fill_sentinel(unsigned char *pixels, int width, int channels,
	int row0, int row1) {
	int y, x;
	for (y = row0; y < row1; y++) {
		for (x = 0; x < width; x++) {
			unsigned char *px = pixels +
				((size_t)y * width + x) * channels;
			px[0] = 255;
			px[1] = 0;
			px[2] = 255;
			if (channels == 4) { px[3] = 255; }
		}
	}
}

void qba_default_config(qba_config *cfg) {
	if (!cfg) { return; }
	cfg->block_height = 16;
	cfg->rle_threshold = 4;
	cfg->index_colors_threshold = 32;
}

void *qba_encode(const void *data, const qoi_desc *desc,
	const qba_config *cfg, int *out_len) {
	qba_config defaults;
	const unsigned char *pixels;
	unsigned char *bytes;
	unsigned int width, height, channels, block_height;
	unsigned int nblocks, b, max_size;
	int p = 0;

	if (
		data == NULL || out_len == NULL || desc == NULL ||
		desc->width == 0 || desc->height == 0 ||
		desc->channels < 3 || desc->channels > 4 ||
		desc->colorspace > 1 ||
		desc->height >= QOI_PIXELS_MAX / desc->width
	) {
		return NULL;
	}

	if (cfg == NULL) {
		qba_default_config(&defaults);
		cfg = &defaults;
	}
	if (
		cfg->block_height == 0 ||
		cfg->rle_threshold == 0 ||
		cfg->index_colors_threshold == 0 ||
		cfg->index_colors_threshold > 255
	) {
		return NULL;
	}

	width = desc->width;
	height = desc->height;
	channels = desc->channels;
	block_height = cfg->block_height;
	nblocks = (height + block_height - 1) / block_height;

	if (nblocks > QBA_BLOCKS_MAX) { return NULL; }

	/* Worst case: every pixel becomes its own RLE control + pixel bytes. */
	max_size = QBA_CONTAINER_HEADER_SIZE + QBA_CONTAINER_END_SIZE +
		nblocks * QBA_BLOCK_HEADER_SIZE +
		(unsigned int)width * height * (channels + 1);

	bytes = (unsigned char *)QOI_MALLOC(max_size);
	if (!bytes) { return NULL; }
	pixels = (const unsigned char *)data;

	memcpy(bytes, "qba1", 4);
	bytes[4] = QBA_VERSION;
	bytes[5] = (unsigned char)channels;
	bytes[6] = desc->colorspace;
	qba_write_32(bytes + 7, width);
	qba_write_32(bytes + 11, height);
	qba_write_32(bytes + 15, block_height);
	bytes[19] = (unsigned char)((cfg->rle_threshold >> 8) & 0xff);
	bytes[20] = (unsigned char)(cfg->rle_threshold & 0xff);
	bytes[21] = (unsigned char)((cfg->index_colors_threshold >> 8) & 0xff);
	bytes[22] = (unsigned char)(cfg->index_colors_threshold & 0xff);
	bytes[23] = 0;
	p = QBA_CONTAINER_HEADER_SIZE;

	for (b = 0; b < nblocks; b++) {
		unsigned int row0 = b * block_height;
		unsigned int row1 = row0 + block_height;
		unsigned int rows, npix, ndist = 0, maxrun = 0;
		const unsigned char *block_px;
		unsigned char mode;
		int payload_len;
		unsigned char *hdr;

		if (row1 > height) { row1 = height; }
		rows = row1 - row0;
		npix = width * rows;
		block_px = pixels + (size_t)row0 * width * channels;

		qba_analyze_block(block_px, channels, (int)npix, &ndist, &maxrun);

		if (maxrun >= cfg->rle_threshold) {
			mode = QBA_MODE_RLE;
		}
		else if (ndist <= cfg->index_colors_threshold) {
			mode = QBA_MODE_INDEX;
		}
		else {
			mode = QBA_MODE_RAW;
		}

		hdr = bytes + p;
		hdr[0] = 'Q';
		hdr[1] = 'B';
		hdr[2] = mode;
		hdr[3] = (unsigned char)((b >> 8) & 0xff);
		hdr[4] = (unsigned char)(b & 0xff);
		p += QBA_BLOCK_HEADER_SIZE;

		if (mode == QBA_MODE_RAW) {
			payload_len = (int)(npix * channels);
			memcpy(bytes + p, block_px, payload_len);
		}
		else if (mode == QBA_MODE_RLE) {
			payload_len = qba_rle_encode(block_px, (int)npix,
				channels, bytes + p);
		}
		else {
			payload_len = qba_index_encode(block_px, (int)npix,
				channels, ndist, bytes + p);
			if (payload_len == 0) { /* shouldn't happen: fall back to raw */
				mode = QBA_MODE_RAW;
				hdr[2] = mode;
				payload_len = (int)(npix * channels);
				memcpy(bytes + p, block_px, payload_len);
			}
		}

		qba_write_32(hdr + 5, (unsigned int)payload_len);
		hdr[9] = qba_crc8(hdr, 9);
		p += payload_len;
	}

	memcpy(bytes + p, "QBAE", 4);
	memset(bytes + p + 4, 0, 4);
	p += QBA_CONTAINER_END_SIZE;

	*out_len = p;
	return bytes;
}

/* Find the next byte position holding a block marker whose header passes the
CRC-8 check and whose block number is exactly want_block. Scanning for the
exact next number (never a later one) prevents payload bytes that happen to
contain a "QB" sequence from being mistaken for a block header. Returns -1
if none. */

static int qba_resync(const unsigned char *bytes, int from, int limit,
	unsigned int want_block) {
	int pos = from;
	while (pos + QBA_BLOCK_HEADER_SIZE <= limit) {
		if (bytes[pos] == 'Q' && bytes[pos + 1] == 'B') {
			unsigned int blocknum =
				((unsigned int)bytes[pos + 3] << 8) | bytes[pos + 4];
			unsigned int payload_len = qba_read_32(bytes + pos + 5);
			if (
				pos > from &&
				qba_crc8(bytes + pos, 9) == bytes[pos + 9] &&
				bytes[pos + 2] <= QBA_MODE_INDEX &&
				blocknum == want_block &&
				(unsigned long)pos + QBA_BLOCK_HEADER_SIZE +
					payload_len <= (unsigned long)limit
			) {
				return pos;
			}
		}
		pos++;
	}
	return -1;
}

void *qba_decode(const void *data, int size, qoi_desc *desc, int channels,
	qba_stats *stats) {
	const unsigned char *bytes;
	unsigned char *pixels, *work;
	unsigned int width, height, out_channels, stored_channels;
	unsigned int block_height, nblocks, expected, pos;
	int stream_end;

	if (
		data == NULL || desc == NULL ||
		(channels != 0 && channels != 3 && channels != 4) ||
		size < QBA_CONTAINER_HEADER_SIZE + QBA_CONTAINER_END_SIZE
	) {
		return NULL;
	}
	bytes = (const unsigned char *)data;

	if (memcmp(bytes, "qba1", 4) != 0 || bytes[4] != QBA_VERSION) {
		return NULL;
	}
	if (bytes[5] != 3 && bytes[5] != 4) { return NULL; }
	if (bytes[6] > 1) { return NULL; }

	width = qba_read_32(bytes + 7);
	height = qba_read_32(bytes + 11);
	block_height = qba_read_32(bytes + 15);

	if (
		width == 0 || height == 0 || block_height == 0 ||
		height >= QOI_PIXELS_MAX / width
	) {
		return NULL;
	}

	out_channels = (channels == 0) ? bytes[5] : (unsigned int)channels;
	stored_channels = bytes[5];
	nblocks = (height + block_height - 1) / block_height;

	desc->width = width;
	desc->height = height;
	desc->channels = stored_channels;
	desc->colorspace = bytes[6];

	pixels = (unsigned char *)QOI_MALLOC(
		(size_t)width * height * out_channels);
	if (!pixels) { return NULL; }

	/* Payloads are always coded with the channel count stored in the
	stream; decode into a work buffer first, then convert. When both counts
	match the output buffer itself is used directly. */
	if (out_channels == stored_channels) {
		work = pixels;
	}
	else {
		work = (unsigned char *)QOI_MALLOC(
			(size_t)width * height * stored_channels);
		if (!work) {
			QOI_FREE(pixels);
			return NULL;
		}
	}

	if (stats) { memset(stats, 0, sizeof(*stats)); }

	stream_end = size - QBA_CONTAINER_END_SIZE;
	expected = 0;
	pos = QBA_CONTAINER_HEADER_SIZE;

	while (expected < nblocks) {
		int header_ok = 0;
		unsigned int mode = 0, blocknum = 0, payload_len = 0;

		if (pos + QBA_BLOCK_HEADER_SIZE <= (unsigned int)stream_end &&
			bytes[pos] == 'Q' && bytes[pos + 1] == 'B' &&
			qba_crc8(bytes + pos, 9) == bytes[pos + 9]
		) {
			mode = bytes[pos + 2];
			blocknum = ((unsigned int)bytes[pos + 3] << 8) | bytes[pos + 4];
			payload_len = qba_read_32(bytes + pos + 5);

			if (
				mode <= QBA_MODE_INDEX &&
				blocknum == expected &&
				(unsigned long)pos + QBA_BLOCK_HEADER_SIZE +
					payload_len <= (unsigned long)stream_end
			) {
				header_ok = 1;
			}
		}

		if (!header_ok) {
			int next;

			if (stats) { stats->corrupted_blocks++; }
			qba_fill_sentinel(work, width, stored_channels,
				expected * block_height,
				(int)((expected + 1) * block_height < height ?
					(expected + 1) * block_height : height));

			/* Resync on the header of the next block number; if it cannot
			be found, every remaining block is damaged as well. The scan
			starts at the current header position: a header whose own CRC
			now fails is skipped, while a still-valid header is retried. */
			next = qba_resync(bytes, (int)pos, stream_end,
				expected + 1);
			if (next < 0) {
				unsigned int b2;
				for (b2 = expected + 1; b2 < nblocks; b2++) {
					if (stats) { stats->corrupted_blocks++; }
					qba_fill_sentinel(work, width, stored_channels,
						b2 * block_height,
						(b2 + 1) * block_height < height ?
							(b2 + 1) * block_height : height);
				}
				expected = nblocks;
				break;
			}

			pos = (unsigned int)next;
			expected++;
			continue;
		}

		/* Header valid: decode the payload in stored channel layout. */
		{
			unsigned int row0 = expected * block_height;
			unsigned int row1 = row0 + block_height;
			unsigned int npix;
			unsigned char *dst;
			int rc = 0;

			if (row1 > height) { row1 = height; }
			npix = width * (row1 - row0);
			dst = work + (size_t)row0 * width * stored_channels;

			if (mode == QBA_MODE_RAW) {
				if (payload_len != npix * stored_channels) {
					rc = -1;
				}
				else {
					memcpy(dst, bytes + pos + QBA_BLOCK_HEADER_SIZE,
						payload_len);
				}
			}
			else if (mode == QBA_MODE_RLE) {
				rc = qba_rle_decode(
					bytes + pos + QBA_BLOCK_HEADER_SIZE,
					(int)payload_len, stored_channels, (int)npix, dst);
			}
			else {
				rc = qba_index_decode(
					bytes + pos + QBA_BLOCK_HEADER_SIZE,
					(int)payload_len, stored_channels, (int)npix, dst);
			}

			if (rc != 0) {
				if (stats) { stats->corrupted_blocks++; }
				qba_fill_sentinel(work, width, stored_channels,
					row0, row1);
			}
			else if (stats) {
				stats->mode_counts[mode]++;
			}
		}

		pos += QBA_BLOCK_HEADER_SIZE + payload_len;
		expected++;
	}

	if (stats) {
		stats->total_blocks = nblocks;
		stats->header_bytes = nblocks * QBA_BLOCK_HEADER_SIZE;
	}

	/* Convert stored channel layout to the requested output layout. */
	if (work != pixels) {
		unsigned int total = width * height;
		unsigned int i;
		for (i = 0; i < total; i++) {
			const unsigned char *s = work + (size_t)i * stored_channels;
			unsigned char *o = pixels + (size_t)i * out_channels;
			o[0] = s[0];
			o[1] = s[1];
			o[2] = s[2];
			if (out_channels == 4) {
				o[3] = (stored_channels == 4) ? s[3] : 255;
			}
		}
		QOI_FREE(work);
	}

	return pixels;
}
#endif /* QOI_IMPLEMENTATION */
