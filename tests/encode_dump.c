/*

Deterministic encoder dump used by tests/run.sh to prove that adding the cache
left qoi_encode() byte-for-byte unchanged.

The same source is compiled twice: once against the pristine qoi.h from git
HEAD, once against the working-tree qoi.h. Each binary writes, for every image
in a fixed corpus, a 4-byte little-endian length followed by the encoded bytes.
The two output files must be identical with `cmp`.

No dependencies beyond libc. */

#define QOI_IMPLEMENTATION
#include "qoi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void put_u32(FILE *f, unsigned int v) {
	unsigned char b[4];
	b[0] = (unsigned char)(v & 0xff);
	b[1] = (unsigned char)((v >> 8) & 0xff);
	b[2] = (unsigned char)((v >> 16) & 0xff);
	b[3] = (unsigned char)((v >> 24) & 0xff);
	fwrite(b, 1, 4, f);
}

static void encode_one(FILE *out, unsigned char *px, int w, int h, int channels,
		int colorspace, unsigned int seed) {
	qoi_desc desc;
	int len = 0;
	unsigned char *e;
	int x, y, c;

	/* Several content classes: noise, gradients, flat runs, edges, and a mix.
	   The mix exercises INDEX/DIFF/LUMA/RUN/RGB/RGBA chunks. */
	for (y = 0; y < h; y++) {
		for (x = 0; x < w; x++) {
			for (c = 0; c < channels; c++) {
				size_t o = ((size_t)y * w + x) * channels + c;
				unsigned int v = (unsigned int)(x * 2654435761u + y * 40503u +
					seed * 2246822519u + c * 3266489917u);
				switch (seed % 5) {
					case 0: px[o] = (unsigned char)(v >> (c * 8)); break;
					case 1: px[o] = (unsigned char)((x * 3 + y * 5 + c * 7) & 0xff); break;
					case 2: px[o] = (unsigned char)((x / 4 + y / 4 + c) & 1 ? 240 : 12); break;
					case 3: px[o] = (unsigned char)((x < w / 2) ? (x * 8 + c) : (y * 8 + c)); break;
					default:
						px[o] = (unsigned char)(((x * y + c * 11) ^ (v >> 24)) & 0xff);
						break;
				}
			}
			if (channels == 4 && seed % 3 == 1) {
				px[((size_t)y * w + x) * channels + 3] =
					(unsigned char)((x / 7 + y / 5) & 1 ? 255 : (30 + x + y));
			}
		}
	}

	desc.width = (unsigned int)w;
	desc.height = (unsigned int)h;
	desc.channels = (unsigned char)channels;
	desc.colorspace = (unsigned char)colorspace;

	e = (unsigned char *)qoi_encode(px, &desc, &len);
	if (!e) {
		/* record failure explicitly with a length of 0xffffffff */
		put_u32(out, 0xffffffffu);
		return;
	}
	put_u32(out, (unsigned int)len);
	fwrite(e, 1, (size_t)len, out);
	free(e);
}

int main(int argc, char **argv) {
	static const int sizes[][2] = {
		{1, 1}, {2, 3}, {8, 8}, {16, 9}, {32, 32}, {37, 23}, {64, 64}, {65, 1}, {1, 65}
	};
	static const int channel_opts[] = {3, 4};
	int si, ci, cs, seed;
	unsigned char *px = NULL;
	size_t cap = 0;
	FILE *out;

	if (argc != 2) {
		fprintf(stderr, "usage: %s <out.bin>\n", argv[0]);
		return 2;
	}
	out = fopen(argv[1], "wb");
	if (!out) {
		perror(argv[1]);
		return 2;
	}

	for (si = 0; si < (int)(sizeof(sizes) / sizeof(sizes[0])); si++) {
		for (ci = 0; ci < 2; ci++) {
			for (cs = 0; cs <= 1; cs++) {
				for (seed = 0; seed < 5; seed++) {
					int w = sizes[si][0], h = sizes[si][1], ch = channel_opts[ci];
					size_t need = (size_t)w * h * ch;
					if (need > cap) {
						unsigned char *np = (unsigned char *)realloc(px, need);
						if (!np) { fclose(out); return 2; }
						px = np;
						cap = need;
					}
					memset(px, 0, need);
					encode_one(out, px, w, h, ch, cs, (unsigned int)seed);
				}
			}
		}
	}

	/* invalid descriptors must fail identically too */
	{
		unsigned char dummy[16];
		qoi_desc d;
		int len = 7;
		void *e;

		memset(dummy, 0, sizeof(dummy));
		d.width = 0; d.height = 4; d.channels = 4; d.colorspace = 0;
		e = qoi_encode(dummy, &d, &len);
		put_u32(out, e ? 1u : 0u);
		free(e);

		d.width = 4; d.width = 4; d.height = 4; d.channels = 2; d.colorspace = 0;
		len = 7;
		e = qoi_encode(dummy, &d, &len);
		put_u32(out, e ? 1u : 0u);
		free(e);

		d.width = 4; d.height = 4; d.channels = 4; d.colorspace = 2;
		len = 7;
		e = qoi_encode(dummy, &d, &len);
		put_u32(out, e ? 1u : 0u);
		free(e);
	}

	free(px);
	if (fclose(out) != 0) {
		return 2;
	}
	return 0;
}
