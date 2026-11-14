/* PNG files, written and read with zlib alone. Pixels are 32-bit RGBA in memory order R, G, B, A.
   The reader takes 8-bit RGB and RGBA images that aren't interlaced, which covers every image this
   program writes. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include "piarcboot.h"

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}

static void chunk(FILE *f, const char *type, const uint8_t *data, uint32_t len)
{
	uint8_t head[8], tail[4];
	uint32_t crc;

	put32(head, len);
	memcpy(head + 4, type, 4);
	crc = crc32(crc32(0, head + 4, 4), data, len);
	put32(tail, crc);
	fwrite(head, 8, 1, f);
	if (len)
		fwrite(data, len, 1, f);
	fwrite(tail, 4, 1, f);
}

int png_write(const char *fn, const uint8_t *rgba, int w, int h)
{
	uLongf zlen = compressBound((uLong)(w * 4 + 1) * h);
	uint8_t *raw = malloc((size_t)(w * 4 + 1) * h), *z = malloc(zlen), ihdr[13];
	FILE *f = fopen(fn, "wb");
	int y, ok = 0;

	if (f && raw && z)
	{
		for (y = 0; y < h; y++)
		{
			raw[(size_t)y * (w * 4 + 1)] = 0;
			memcpy(raw + (size_t)y * (w * 4 + 1) + 1, rgba + (size_t)y * w * 4, w * 4);
		}
		compress2(z, &zlen, raw, (uLong)(w * 4 + 1) * h, 9);
		put32(ihdr, w);
		put32(ihdr + 4, h);
		memcpy(ihdr + 8, "\10\6\0\0\0", 5);
		fwrite("\x89PNG\r\n\x1a\n", 8, 1, f);
		chunk(f, "IHDR", ihdr, 13);
		chunk(f, "IDAT", z, zlen);
		chunk(f, "IEND", NULL, 0);
		ok = !ferror(f);
	}
	if (f)
		fclose(f);
	free(raw);
	free(z);
	return ok ? 0 : -1;
}

static int paeth(int a, int b, int c)
{
	int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);

	return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

/* Returns the pixels, to be freed, or NULL */
uint8_t *png_read(const char *fn, int *w, int *h)
{
	size_t len, at = 8, zlen = 0;
	uint8_t *d = file_read(fn, &len), *z = NULL, *raw = NULL, *out = NULL;
	int bpp = 0, x, y, i;
	uLongf rawlen;

	if (!d || len < 33 || memcmp(d, "\x89PNG\r\n\x1a\n", 8))
		goto done;
	z = malloc(len);
	while (at + 12 <= len)
	{
		uint32_t n = (uint32_t)d[at] << 24 | d[at + 1] << 16 | d[at + 2] << 8 | d[at + 3];

		if (n > len - at - 12)
			break;
		if (!memcmp(d + at + 4, "IHDR", 4) && n >= 13)
		{
			*w = d[at + 8] << 24 | d[at + 9] << 16 | d[at + 10] << 8 | d[at + 11];
			*h = d[at + 12] << 24 | d[at + 13] << 16 | d[at + 14] << 8 | d[at + 15];
			if (d[at + 16] != 8 || d[at + 20] || *w <= 0 || *h <= 0 || *w > 8192 || *h > 8192)
				goto done;
			bpp = d[at + 17] == 6 ? 4 : d[at + 17] == 2 ? 3 : 0;
		}
		else if (!memcmp(d + at + 4, "IDAT", 4))
		{
			memcpy(z + zlen, d + at + 8, n);
			zlen += n;
		}
		at += n + 12;
	}
	if (!bpp)
		goto done;
	rawlen = (uLongf)(*w * bpp + 1) * *h;
	raw = malloc(rawlen);
	if (uncompress(raw, &rawlen, z, zlen) != Z_OK || rawlen != (uLongf)(*w * bpp + 1) * *h)
		goto done;
	out = malloc((size_t)*w * *h * 4);
	for (y = 0; y < *h; y++)
	{
		uint8_t *row = raw + (size_t)y * (*w * bpp + 1) + 1, *up = y ? row - (*w * bpp + 1) : NULL;
		int filter = row[-1];

		for (i = 0; i < *w * bpp; i++)
		{
			int a = i >= bpp ? row[i - bpp] : 0, b = up ? up[i] : 0, c = up && i >= bpp ? up[i - bpp] : 0;

			row[i] += filter == 1 ? a : filter == 2 ? b : filter == 3 ? (a + b) / 2 : filter == 4 ? paeth(a, b, c) : 0;
		}
		for (x = 0; x < *w; x++)
		{
			uint8_t *o = out + ((size_t)y * *w + x) * 4;

			memcpy(o, row + x * bpp, bpp);
			if (bpp == 3)
				o[3] = 255;
		}
	}
done:
	free(d);
	free(z);
	free(raw);
	return out;
}
