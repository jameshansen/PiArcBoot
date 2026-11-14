/* Icons. A game's icon is its own RISC OS sprite, the one the desktop showed for it, set in a
   circle with a pale ring and a soft shadow. This file reads sprite files, finds them inside
   disc images, and draws the circle, along with the two icons that have no sprite behind them:
   the plain floppy disc for a game without one, and the A that is PiArcBoot's own. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "piarcboot.h"

static uint32_t rd32(const uint8_t *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

/* The desktop's 16 colours, which a 16 colour sprite with no palette of its own is drawn in */
static const uint32_t wimp16[16] = {
	0xffffff, 0xdddddd, 0xbbbbbb, 0x999999, 0x777777, 0x555555, 0x333333, 0x000000,
	0x004499, 0xeeee00, 0x00cc00, 0xdd0000, 0xeeeebb, 0x558800, 0xffbb00, 0x00bbff
};

/* The fixed 256 colours of an Archimedes: 4 bits each of red, green and blue, with the low 2
   shared between them as the tint */
static uint32_t colour256(int c)
{
	int tint = c & 3;
	int r = (c & 0x10) >> 1 | (c & 0x04) | tint;
	int g = (c & 0x40) >> 3 | (c & 0x20) >> 3 | tint;
	int b = (c & 0x80) >> 4 | (c & 0x08) >> 1 | tint;

	return (r * 17) << 16 | (g * 17) << 8 | b * 17;
}

/* Bits a pixel, and how many screen units a pixel is wide and high, for a sprite's mode */
static int mode_info(uint32_t mode, int *px, int *py)
{
	static const struct { uint8_t mode, bpp, xe, ye; } m[] = {
		{ 0, 1, 1, 2 }, { 1, 2, 2, 2 }, { 2, 4, 3, 2 }, { 4, 1, 2, 2 }, { 5, 2, 3, 2 }, { 8, 2, 1, 2 },
		{ 9, 4, 2, 2 }, { 10, 8, 3, 2 }, { 11, 2, 1, 2 }, { 12, 4, 1, 2 }, { 13, 8, 2, 2 }, { 14, 4, 1, 2 },
		{ 15, 8, 1, 2 }, { 16, 4, 1, 2 }, { 17, 4, 1, 2 }, { 18, 1, 1, 1 }, { 19, 2, 1, 1 }, { 20, 4, 1, 1 },
		{ 21, 8, 1, 1 }, { 23, 1, 1, 1 }, { 24, 8, 1, 2 }, { 25, 1, 1, 1 }, { 26, 2, 1, 1 }, { 27, 4, 1, 1 },
		{ 28, 8, 1, 1 }, { 29, 1, 1, 1 }, { 30, 2, 1, 1 }, { 31, 4, 1, 1 }, { 32, 8, 1, 1 }, { 33, 1, 1, 2 },
		{ 34, 2, 1, 2 }, { 35, 4, 1, 2 }, { 36, 8, 1, 2 }, { 39, 4, 1, 1 }, { 40, 8, 1, 1 } };
	size_t i;

	if (mode >= 256)                /* a mode word: the type and the dots an inch */
	{
		int type = mode >> 27, xdpi = (mode >> 1) & 0x1fff, ydpi = (mode >> 14) & 0x1fff;

		*px = xdpi >= 180 ? 1 : xdpi >= 90 ? 2 : 4;
		*py = ydpi >= 180 ? 1 : ydpi >= 90 ? 2 : 4;
		return type >= 1 && type <= 4 ? 1 << (type - 1) : 0;
	}
	for (i = 0; i < sizeof m / sizeof *m; i++)
		if (m[i].mode == mode)
		{
			*px = 1 << m[i].xe;
			*py = 1 << m[i].ye;
			return m[i].bpp;
		}
	return 0;
}

/* One sprite from a sprite file, at p with n bytes left. Returns 0 if it isn't a sprite this
   program can draw. The picture comes out stretched to square pixels. */
static int sprite_read(const uint8_t *p, size_t n, sprite_t *s)
{
	uint32_t next, wwords, lines, first, last, image, mask, mode;
	int bpp, px, py, w, h, x, y, row, ncol, i, sx, sy;
	uint32_t pal[256];
	uint8_t *raw;

	if (n < 44)
		return 0;
	next = rd32(p); wwords = rd32(p + 16) + 1; lines = rd32(p + 20) + 1;
	first = rd32(p + 24); last = rd32(p + 28); image = rd32(p + 32); mask = rd32(p + 36); mode = rd32(p + 40);
	bpp = mode_info(mode, &px, &py);
	if (!bpp || next < 44 || next > n || wwords > 512 || lines > 2048 || first > 31 || last > 31 ||
	    image < 44 || image >= next || mask < 44 || mask >= next)
		return 0;
	row = wwords * 4;
	w = (wwords * 32 - first - (31 - last)) / bpp;
	h = lines;
	if (w <= 0 || image + (size_t)row * h > next)
		return 0;
	for (i = 0; i < 12; i++)
		if (p[4 + i] && (p[4 + i] < 33 || (p[4 + i] > 126 && p[4 + i] < 160)))  /* RISC OS's Latin-1 letters are fine */
			return 0;
	memcpy(s->name, p + 4, 12);
	s->name[12] = 0;

	ncol = (image - 44) / 8;
	for (i = 0; i < 256; i++)
		pal[i] = bpp == 8 ? colour256(i) : bpp == 4 ? wimp16[i & 15] :
			 bpp == 2 ? wimp16[(i & 3) == 3 ? 7 : (i & 3) * 2] : wimp16[i & 1 ? 7 : 0];
	if (ncol >= (1 << bpp) || (bpp == 8 && ncol == 256))
		for (i = 0; i < ncol && i < 256; i++)   /* its own palette, each entry &BBGGRR00 */
		{
			uint32_t e = rd32(p + 44 + i * 8);

			pal[i] = (e >> 8 & 255) << 16 | (e >> 16 & 255) << 8 | e >> 24;
		}

	raw = malloc((size_t)w * h * 4);
	s->masked = 0;
	for (y = 0; y < h; y++)
		for (x = 0; x < w; x++)
		{
			int bit = first + x * bpp;
			const uint8_t *q = p + image + (size_t)y * row + bit / 8;
			int v = (q[0] | (bit / 8 + 1 < row ? q[1] << 8 : 0)) >> (bit & 7) & ((1 << bpp) - 1);
			uint8_t *o = raw + ((size_t)y * w + x) * 4;
			int a = 255;

			if (mask != image && mask + (size_t)row * h <= next && mode < 256)
			{
				q = p + mask + (size_t)y * row + bit / 8;
				if (!((q[0] | (bit / 8 + 1 < row ? q[1] << 8 : 0)) >> (bit & 7) & ((1 << bpp) - 1)))
					a = 0;
			}
			if (!a)
				s->masked = 1;
			o[0] = pal[v] >> 16; o[1] = pal[v] >> 8; o[2] = pal[v]; o[3] = a;
		}

	/* to square pixels: each pixel becomes px by py, divided down by what they share */
	sx = px; sy = py;
	while (!(sx & 1) && !(sy & 1)) { sx >>= 1; sy >>= 1; }
	s->w = w * sx;
	s->h = h * sy;
	s->rgba = malloc((size_t)s->w * s->h * 4);
	for (y = 0; y < s->h; y++)
		for (x = 0; x < s->w; x++)
			memcpy(s->rgba + ((size_t)y * s->w + x) * 4, raw + ((size_t)(y / sy) * w + x / sx) * 4, 4);
	free(raw);
	return 1;
}

/* Every sprite in a sprite file. Returns how many, with *out to be freed by sprites_free. */
int sprites_parse(const uint8_t *d, size_t len, sprite_t **out)
{
	uint32_t count, at;
	sprite_t *list;
	int n = 0;

	*out = NULL;
	if (len < 12 + 44)
		return 0;
	count = rd32(d);
	at = rd32(d + 4) - 4;           /* offsets count from a word that isn't in the file */
	if (count < 1 || count > 512 || at < 12 || at >= len)
		return 0;
	list = calloc(count, sizeof *list);
	while (count-- && at + 44 <= len)
	{
		uint32_t next = rd32(d + at);

		if (next < 44 || next > len - at)
			break;
		if (sprite_read(d + at, len - at, &list[n]))
			n++;
		at += next;
	}
	if (!n)
	{
		free(list);
		return 0;
	}
	*out = list;
	return n;
}

void sprites_free(sprite_t *list, int n)
{
	while (n-- > 0)
		free(list[n].rgba);
	free(list);
}

/* The sprites of every sprite file found in a disc image, for picking a game's icon from. A
   sprite file is looked for at each 256 byte boundary, by the shape of its header.
   A file the disc holds in more than one piece is missed. */
int sprites_scan(const uint8_t *img, size_t len, sprite_t **out)
{
	sprite_t *all = NULL;
	size_t at;
	int total = 0;

	for (at = 0; at + 12 + 44 <= len; at += 256)
	{
		uint32_t count = rd32(img + at), first = rd32(img + at + 4), free_at = rd32(img + at + 8);
		sprite_t *list;
		int n, i;

		if (count < 1 || count > 64 || first != 16 || free_at < 60 || free_at > 400000 || at + free_at - 4 > len)
			continue;
		n = sprites_parse(img + at, free_at - 4, &list);
		for (i = 0; i < n; i++)
		{
			int j, dup = 0;

			for (j = 0; j < total; j++)
				dup |= !strcmp(all[j].name, list[i].name) && all[j].w == list[i].w && all[j].h == list[i].h &&
				       !memcmp(all[j].rgba, list[i].rgba, (size_t)list[i].w * list[i].h * 4);
			if (dup || total >= 96)
			{
				free(list[i].rgba);
				continue;
			}
			all = realloc(all, (total + 1) * sizeof *all);
			all[total++] = list[i];
		}
		if (n)
			free(list);
	}
	*out = all;
	return total;
}

/* The sprite that stands for a game: the large application sprite, which is named after the
   application ("!lemmings"), ahead of its small form ("sm!lemmings") and anything else */
int sprites_main(const sprite_t *list, int n)
{
	int i, best = 0, best_score = -1;

	for (i = 0; i < n; i++)
	{
		int score = (list[i].name[0] == '!' ? 4 : 0) + (strncmp(list[i].name, "sm!", 3) ? 2 : 0) +
			    (list[i].w >= 24 && list[i].w <= 80 ? 1 : 0);

		if (score > best_score)
		{
			best_score = score;
			best = i;
		}
	}
	return best;
}

/* The circle. Its ring, shading and shadow are the same on every icon: the ring is 5 pixels
   wide around a picture 111 across, white at the top and grey at the bottom, a little lighter
   at its edges, and the shadow falls just below. Each pixel is worked out from 16 samples. */
#define ICON 128
#define CX 64.5
#define CY 62.5
#define R_OUT 60.5
#define R_IN 55.5

static void circle(uint8_t *out, void (*inside)(double x, double y, double *rgb, void *p), void *p)
{
	int x, y, i, j;

	for (y = 0; y < ICON; y++)
		for (x = 0; x < ICON; x++)
		{
			double acc[3] = { 0, 0, 0 }, cover = 0, shadow = 0;
			uint8_t *o = out + (y * ICON + x) * 4;

			for (j = 0; j < 4; j++)
				for (i = 0; i < 4; i++)
				{
					double sx = x + (i + 0.5) / 4, sy = y + (j + 0.5) / 4;
					double d = hypot(sx - CX, sy - CY), ds = hypot(sx - CX, sy - (CY + 2.5));
					double rgb[3];

					if (d <= R_IN)
						inside(sx, sy, rgb, p);
					else if (d <= R_OUT)
					{
						double t = (sy - (CY - R_OUT)) / (2 * R_OUT);
						double edge = d - R_IN < 1 || R_OUT - d < 1 ? 8 : 0;

						rgb[0] = 246 - 58 * t + edge;
						rgb[1] = 246 - 58 * t + edge;
						rgb[2] = 249 - 58 * t + edge;
					}
					else
					{
						double s = (R_OUT + 3.5 - ds) / 6;      /* the shadow's soft edge */

						shadow += s <= 0 ? 0 : s >= 1 ? 0.5 : 0.5 * s * s * (3 - 2 * s);
						continue;
					}
					acc[0] += rgb[0]; acc[1] += rgb[1]; acc[2] += rgb[2];
					cover++;
				}
			if (cover)
				for (i = 0; i < 3; i++)
				{
					double v = acc[i] / cover;

					o[i] = v < 0 ? 0 : v > 255 ? 255 : (uint8_t)(v + 0.5);
				}
			else
				o[0] = o[1] = o[2] = 0;
			o[3] = (uint8_t)((cover + shadow) / 16 * 255 + 0.5);
			if (cover && cover < 16)        /* an edge pixel: the picture over the shadow */
				for (i = 0; i < 3; i++)
					o[i] = (uint8_t)(o[i] * cover / (cover + shadow) + 0.5);
		}
}

typedef struct
{
	const sprite_t *s;
	int x0, y0, x1, y1;             /* the part of the sprite that has anything in it */
	double scale, top[3], bottom[3];
	const float *back;              /* what is behind the sprite, ICON by ICON RGB, or NULL for the tint alone */
} fill_t;

/* How much of the sprite shows at a distance from the middle of the circle: all of it most of
   the way out, then less and less, down to none at the ring */
#define FADE_FROM 0.74
#define FADE_TO 1.0

static void fill_sprite(double x, double y, double *rgb, void *p)
{
	const fill_t *f = p;
	double t = (y - (CY - R_IN)) / (2 * R_IN);
	double u = (x - CX) / f->scale + (f->x0 + f->x1) / 2.0, v = (y - CY) / f->scale + (f->y0 + f->y1) / 2.0;
	int i;

	if (f->back)
	{
		int bx = x < 0 ? 0 : x >= ICON ? ICON - 1 : (int)x, by = y < 0 ? 0 : y >= ICON ? ICON - 1 : (int)y;

		for (i = 0; i < 3; i++)
			rgb[i] = f->back[(by * ICON + bx) * 3 + i];
	}
	else
		for (i = 0; i < 3; i++)
			rgb[i] = f->top[i] + (f->bottom[i] - f->top[i]) * t;
	if (f->s && u >= f->x0 && u < f->x1 && v >= f->y0 && v < f->y1)
	{
		const uint8_t *q = f->s->rgba + ((size_t)(int)v * f->s->w + (int)u) * 4;

		if (q[3])
		{
			double d = hypot(x - CX, y - CY) / R_IN, m = 1;

			if (f->back && d > FADE_FROM)
			{
				m = d >= FADE_TO ? 0 : (FADE_TO - d) / (FADE_TO - FADE_FROM);
				m = m * m * (3 - 2 * m);
			}
			for (i = 0; i < 3; i++)
				rgb[i] += (q[i] - rgb[i]) * m;
		}
	}
}

/* What goes behind a sprite in its circle: the sprite again, large enough to cover the whole
   circle and blurred until it is only its colours, over the tint where it has none. It is
   lightened a little, so that a sprite of one colour still stands out from it. */
#define LIGHTEN 0.3
static float *backdrop(const fill_t *f)
{
	float *px = calloc(ICON * ICON, 4 * sizeof(float)), *tmp = malloc(ICON * ICON * 4 * sizeof(float));
	float *back = malloc(ICON * ICON * 3 * sizeof(float));
	int w = f->x1 - f->x0, h = f->y1 - f->y0, x, y, c, pass;
	double cover = 2 * R_OUT * 1.3 / (w < h ? w : h);
	const int r = 7;

	for (y = 0; y < ICON; y++)
		for (x = 0; x < ICON; x++)
		{
			double u = (x + 0.5 - CX) / cover + (f->x0 + f->x1) / 2.0, v = (y + 0.5 - CY) / cover + (f->y0 + f->y1) / 2.0;
			const uint8_t *q;

			if (u < f->x0 || u >= f->x1 || v < f->y0 || v >= f->y1)
				continue;
			q = f->s->rgba + ((size_t)(int)v * f->s->w + (int)u) * 4;
			if (q[3])
				for (c = 0; c < 4; c++)
					px[(y * ICON + x) * 4 + c] = c < 3 ? q[c] : 1;
		}
	/* 3 box blurs each way come to a smooth one. Colour is carried weighted by how much of it
	   there is, so the empty parts of the sprite don't darken what is beside them. */
	for (pass = 0; pass < 6; pass++)
	{
		int dx = pass & 1 ? 0 : 1, dy = pass & 1 ? 1 : 0;

		for (y = 0; y < ICON; y++)
			for (x = 0; x < ICON; x++)
				for (c = 0; c < 4; c++)
				{
					float sum = 0;
					int k;

					for (k = -r; k <= r; k++)
					{
						int xx = x + k * dx, yy = y + k * dy;

						xx = xx < 0 ? 0 : xx >= ICON ? ICON - 1 : xx;
						yy = yy < 0 ? 0 : yy >= ICON ? ICON - 1 : yy;
						sum += px[(yy * ICON + xx) * 4 + c];
					}
					tmp[(y * ICON + x) * 4 + c] = sum / (2 * r + 1);
				}
		memcpy(px, tmp, ICON * ICON * 4 * sizeof(float));
	}
	for (y = 0; y < ICON; y++)
		for (x = 0; x < ICON; x++)
		{
			double t = (y + 0.5 - (CY - R_IN)) / (2 * R_IN), a = px[(y * ICON + x) * 4 + 3];

			for (c = 0; c < 3; c++)
			{
				double v = px[(y * ICON + x) * 4 + c] + (1 - a) * (f->top[c] + (f->bottom[c] - f->top[c]) * t);

				back[(y * ICON + x) * 3 + c] = v + (255 - v) * LIGHTEN;
			}
		}
	free(px);
	free(tmp);
	return back;
}

/* A 128 by 128 RGBA icon for a sprite. The sprite sits in the middle of the circle, a sprite
   with a shape of its own a little smaller than one that is a full rectangle. Behind it is the
   same sprite enlarged and blurred, and towards the ring the sprite fades into that. */
void icon_render(uint8_t *out, const sprite_t *s)
{
	fill_t f = { s, 0, 0, 0, 0, 1, { 191, 233, 255 }, { 234, 248, 255 }, NULL };
	int x, y, i, best = -1, best_n = 0, hist[512] = { 0 };
	float *back = NULL;

	if (s)
	{
		f.x0 = s->w; f.y0 = s->h;
		for (y = 0; y < s->h; y++)
			for (x = 0; x < s->w; x++)
			{
				const uint8_t *q = s->rgba + ((size_t)y * s->w + x) * 4;
				int hi = q[0] > q[1] ? (q[0] > q[2] ? q[0] : q[2]) : (q[1] > q[2] ? q[1] : q[2]);
				int lo = q[0] < q[1] ? (q[0] < q[2] ? q[0] : q[2]) : (q[1] < q[2] ? q[1] : q[2]);

				if (!q[3])
					continue;
				if (x < f.x0) f.x0 = x;
				if (y < f.y0) f.y0 = y;
				if (x >= f.x1) f.x1 = x + 1;
				if (y >= f.y1) f.y1 = y + 1;
				if (hi - lo > 80)       /* a strong colour */
					hist[(q[0] >> 5) << 6 | (q[1] >> 5) << 3 | q[2] >> 5]++;
			}
		if (f.x1 <= f.x0 || f.y1 <= f.y0)
			f.s = NULL;
	}
	if (f.s)
	{
		int w = f.x1 - f.x0, h = f.y1 - f.y0;

		f.scale = (s->masked ? 2 * R_IN * 0.86 : 2 * R_IN) / (w > h ? w : h);
		for (i = 0; i < 512; i++)
			if (hist[i] > best_n)
			{
				best_n = hist[i];
				best = i;
			}
		if (best >= 0 && best_n * 12 > w * h)   /* the tint, for where the sprite has nothing: a pale one of its commonest strong colour */
		{
			double c[3] = { (best >> 6) * 36.4, (best >> 3 & 7) * 36.4, (best & 7) * 36.4 };

			for (i = 0; i < 3; i++)
			{
				f.top[i] = 255 - (255 - c[i]) * 0.22;
				f.bottom[i] = 255 - (255 - c[i]) * 0.42;
			}
		}
		f.back = back = backdrop(&f);
	}
	circle(out, fill_sprite, &f);
	free(back);
}

/* The icon of a game that has no sprite: a 3.5 inch floppy disc, drawn here from its shapes. A
   dark blue square with its top right corner cut off, the metal shutter at the top with its
   window, a white label below with ruled lines, and the write protect hole at the bottom left. */
static void fill_floppy(double x, double y, double *rgb, void *p)
{
	const double side = 1.30 * R_IN, cut = 0.14, line = 1.3 / side;
	double u = (x - (CX - side / 2)) / side, v = (y - (CY - side / 2)) / side;
	double t = (y - (CY - R_IN)) / (2 * R_IN);

	(void)p;
#define SET(r, g, b) (rgb[0] = (r), rgb[1] = (g), rgb[2] = (b))
	SET(191 + 43 * t, 233 + 15 * t, 255);           /* the pale ground round it */
	if (u < 0 || u > 1 || v < 0 || v > 1 || u - v > 1 - cut)
		return;
	if (u < line || u > 1 - line || v < line || v > 1 - line || u - v > 1 - cut - line * 1.4142)
		SET(14, 28, 56);                        /* its edge */
	else if (u > 0.24 && u < 0.76 && v < 0.38)      /* the shutter */
	{
		if (u > 0.53 && u < 0.66 && v > 0.07 && v < 0.30)
			SET(34, 38, 46);                /* its window */
		else if (u < 0.24 + line || u > 0.76 - line || v > 0.38 - line)
			SET(110, 116, 124);
		else
			SET(214 - 50 * v, 218 - 50 * v, 224 - 50 * v);
	}
	else if (u > 0.13 && u < 0.87 && v > 0.48 && v < 0.95)  /* the label */
	{
		if (u > 0.2 && u < 0.8 && (fabs(v - 0.62) < 0.012 || fabs(v - 0.72) < 0.012 || fabs(v - 0.82) < 0.012))
			SET(190, 200, 218);
		else
			SET(246, 246, 242);
	}
	else if (u > 0.04 && u < 0.10 && v > 0.86 && v < 0.93)
		SET(12, 20, 40);                        /* the write protect hole */
	else
		SET(44 - 16 * v, 88 - 30 * v, 168 - 48 * v);    /* the body, darker towards its foot */
#undef SET
}

void icon_generic(uint8_t *out)
{
	circle(out, fill_floppy, NULL);
}

/* PiArcBoot's own icon: the Archimedes's badge, drawn here from its shapes. The A is a white
   triangle with its upright side on the right, on the badge's blue. Inside it a band of white,
   an arc of a ring, parts a red top from a yellow foot. As on the badge, the triangle runs off
   the bottom edge. */
static void fill_badge(double x, double y, double *rgb, void *p)
{
	static const double blue[3] = { 1, 154, 222 }, red[3] = { 246, 86, 74 }, yellow[3] = { 242, 206, 46 };
	const double size = 102.0 / 237, slope = 183.9 / 237;   /* the badge's triangle is 203 across and 237 up */
	double u = (x - (CX + 30 - 203.2 * size)) / size, v = (y - (CY - 44)) / size;
	double t = (y - (CY - R_IN)) / (2 * R_IN), ring = u - 144.5;
	const double *c = NULL;
	int i;

	(void)p;
	for (i = 0; i < 3; i++)
		rgb[i] = blue[i] + 26 - 44 * t;         /* lighter at the top, as the ring round it is */
	if (v < 0 || v > 237 || u > 203.2 || u < 183.5 - slope * v)
		return;                                 /* outside the triangle */
	for (i = 0; i < 3; i++)
		rgb[i] = 255;
	if (u > 186.4 || u < 186.4 - slope * (v - 24.5))
		return;                                 /* its white edge */
	if (fabs(ring) < 80.5 && v < 180 - sqrt(80.5 * 80.5 - ring * ring))
		c = red;                                /* above the band */
	else if (u < 108.3 || v > 180 - sqrt(65 * 65 - ring * ring))
		c = yellow;                             /* below it, and all that is left of where it meets the slope */
	if (c)
		for (i = 0; i < 3; i++)
			rgb[i] = c[i];
}

void icon_app(uint8_t *out)
{
	circle(out, fill_badge, NULL);
}
