/* PiArcBoot: disc images, read into the sectors of each track side as a drive sees them.

   JFD is JASPP's format, read as JASPP describes it: each sector with its size, its ID, CRC
   errors in its ID or data field, its density and where it starts on the track. HFE is the HxC
   Floppy Emulator's bit level format: each side's MFM cell stream, decoded here into sectors as
   a 1772 would find them. ADF, ADL and the other flat images are told apart by their size.

   A track side is laid out as the WD1772 data sheet's IBM format has it: before each sector 12
   bytes of 00 and three A1 sync bytes, the ID address mark FE, the track, side, sector and size
   code, two CRC bytes, a 22 byte gap, 12 bytes of 00, three A1s, the data address mark FB, the
   data and its CRC. A sector whose place the image gives starts there; the rest are spread
   evenly round the track.

   The images are never written to. What a game writes goes in <image>.sav, a whole track side at
   a time, and is put back over the image when it is next loaded.

   Free software under the GNU General Public License, version 2 or (at your option) any later
   version. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include "piarcboot.h"
#include "disc.h"

#define SAV_MAGIC "PIARCBOOT-SAV 1\n"

static uint32_t rd32(const uint8_t *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static unsigned rd16(const uint8_t *p)
{
	return p[0] | p[1] << 8;
}

static void wr32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static void wr16(uint8_t *p, unsigned v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

/* CRC-CCITT, polynomial &1021, as the 1772 computes it from &FFFF over the address marks and
   the bytes after them */
/* A side as a format program lays one down: double or high density sectors all of one size,
   numbered for this cylinder and side, with good IDs and data, spaced evenly round a track of
   the usual length. Copy protection makes the other kind. */
int disc_ordinary(const dside_t *side, int cyl, int head)
{
	int rev, gap_min, gap_max = 0, i, k;

	if (!side || side->n < 1 || side->n > 32 || (side->s[0].density != DENS_DOUBLE && side->s[0].density != DENS_HIGH))
		return 0;
	rev = side->s[0].density == DENS_HIGH ? DISC_HD_BYTES : DISC_DD_BYTES;
	if (side->bytes && side->bytes != rev)
		return 0;
	gap_min = rev;
	for (i = 0; i < side->n; i++)
	{
		const dsector_t *s = &side->s[i];
		int gap = (i + 1 < side->n ? side->s[i + 1].pos : side->s[0].pos + rev) - s->pos;

		if (s->id_crc || s->data_crc || s->density != side->s[0].density || s->c != cyl || s->h != head || s->pos < 0 ||
		    s->n > 3 || s->n != side->s[0].n || s->len != 128u << s->n)
			return 0;
		for (k = 0; k < i; k++)
			if (side->s[k].r == s->r)
				return 0;
		gap_min = gap < gap_min ? gap : gap_min;
		gap_max = gap > gap_max ? gap : gap_max;
	}
	return gap_min > 0 && gap_max - gap_min <= (gap_min + gap_max) / 8;      /* within about 12% of each other */
}

uint16_t disc_crc(uint16_t crc, const uint8_t *p, size_t n)
{
	int i;

	while (n--)
	{
		crc ^= (uint16_t)(*p++ << 8);
		for (i = 0; i < 8; i++)
			crc = crc & 0x8000 ? (uint16_t)(crc << 1 ^ 0x1021) : (uint16_t)(crc << 1);
	}
	return crc;
}

uint16_t disc_id_crc(const dsector_t *s)
{
	uint8_t b[8] = { 0xa1, 0xa1, 0xa1, 0xfe, s->c, s->h, s->r, s->n };

	return disc_crc(0xffff, b, 8);
}

uint16_t disc_data_crc(const uint8_t *d, size_t n, uint8_t mark)
{
	uint8_t b[4] = { 0xa1, 0xa1, 0xa1, mark };

	return disc_crc(disc_crc(0xffff, b, 4), d, n);
}

/* A gzip file's contents, or NULL if it isn't one or is damaged. A file may be several gzip
   members one after another, as some of JASPP's are, and their contents follow on. */
static uint8_t *gunzip(const uint8_t *d, size_t len, size_t *out_len)
{
	z_stream z;
	size_t cap = len * 4 + 65536, done = 0;        /* done: what the members before this one gave */
	uint8_t *out;
	int r;

	if (len < 18 || d[0] != 0x1f || d[1] != 0x8b)
		return NULL;
	memset(&z, 0, sizeof z);
	if (inflateInit2(&z, 15 + 32) != Z_OK)
		return NULL;
	out = malloc(cap);
	z.next_in = (Bytef *)d;
	z.avail_in = (uInt)len;
	for (;;)
	{
		z.next_out = out + done + z.total_out;
		z.avail_out = (uInt)(cap - done - z.total_out);
		r = inflate(&z, Z_NO_FLUSH);
		if (r == Z_STREAM_END)
		{
			done += z.total_out;
			if (z.avail_in < 18 || z.next_in[0] != 0x1f || z.next_in[1] != 0x8b)
				break;
			inflateReset(&z);       /* another member follows */
			continue;
		}
		if ((r != Z_OK && r != Z_BUF_ERROR) || (r == Z_BUF_ERROR && z.avail_out))
		{
			inflateEnd(&z);
			free(out);
			return NULL;
		}
		if (!z.avail_out)
		{
			cap *= 2;
			if (cap > 64 << 20)     /* far more than any floppy image */
			{
				inflateEnd(&z);
				free(out);
				return NULL;
			}
			out = realloc(out, cap);
		}
	}
	*out_len = done;
	inflateEnd(&z);
	return out;
}

uint8_t *jfd_read(const char *path, size_t *len)
{
	size_t n, m;
	uint8_t *d = file_read(path, &n), *u;

	if (!d)
		return NULL;
	u = gunzip(d, n, &m);
	if (u)
	{
		free(d);
		d = u;
		n = m;
	}
	if (len)
		*len = n;
	return d;
}

/* ---- Track layout ---- */

static int by_pos(const void *a, const void *b)
{
	const dsector_t *s = a, *t = b;

	return s->pos != t->pos ? s->pos - t->pos : 0;
}

/* Bytes a sector takes on the track from its first sync byte to the end of its data CRC */
static int footprint(const dsector_t *s)
{
	return 15 + 7 + 22 + 16 + (int)s->len + 2;
}

void disc_layout(dside_t *side)
{
	int bytes = side->bytes ? side->bytes : DISC_DD_BYTES, i, given = 0, at, gap3 = 0, total = 0;

	side->bytes = bytes;
	if (!side->n)
		return;
	for (i = 0; i < side->n; i++)
	{
		given += side->s[i].pos >= 0;
		total += footprint(&side->s[i]);
	}
	if (!given)
	{
		/* Evenly round the track, after a first gap */
		at = 60;
		if (bytes - at - total > 0)
			gap3 = (bytes - at - total) / side->n;
		else
			at = 16;
		for (i = 0; i < side->n; i++)
		{
			side->s[i].pos = at + 15;
			at += footprint(&side->s[i]) + gap3;
		}
	}
	else
		for (i = 0; i < side->n; i++)
			if (side->s[i].pos < 0)         /* after the one before, with a short gap */
				side->s[i].pos = i ? side->s[i - 1].pos + footprint(&side->s[i - 1]) - 15 + 24 + 15 : 75;
	for (i = 0; i < side->n; i++)
		side->s[i].pos %= bytes;
	/* In the order they pass the head. A stable sort keeps sectors at one place in the image's order. */
	for (i = 1; i < side->n; i++)
	{
		dsector_t t = side->s[i];
		int k = i;

		while (k > 0 && by_pos(&side->s[k - 1], &t) > 0)
		{
			side->s[k] = side->s[k - 1];
			k--;
		}
		side->s[k] = t;
	}
}

static void side_free(dside_t *side)
{
	int i;

	for (i = 0; i < side->n; i++)
		free(side->s[i].data);
	free(side->s);
	side->s = NULL;
	side->n = 0;
}

void disc_free(disc_t *d)
{
	int c, h;

	if (!d)
		return;
	for (c = 0; c < DISC_CYLS; c++)
		for (h = 0; h < 2; h++)
			side_free(&d->side[c][h]);
	free(d);
}

void disc_set_side(disc_t *d, int cyl, int head, dsector_t *s, int n, int bytes)
{
	dside_t *side;

	if (cyl < 0 || cyl >= DISC_CYLS || head < 0 || head > 1)
		return;
	side = &d->side[cyl][head];
	side_free(side);
	side->s = s;
	side->n = n;
	side->bytes = bytes;
	disc_layout(side);
	side->changed = 1;
	d->changed = 1;
	if (n && cyl + 1 > d->cyls)
		d->cyls = cyl + 1;
}

/* ---- JFD ---- */

static int is_jfd(const uint8_t *d, size_t len)
{
	return len >= 48 && !memcmp(d, "JFDI", 4);
}

/* The track table: where it is and how many sides it lists. Returns 0 if it makes no sense. The
   track table, the sector table and the data follow one another. */
static int jfd_tables(const uint8_t *d, size_t len, uint32_t *tt, uint32_t *st, uint32_t *da, int *sides)
{
	*tt = rd32(d + 24);
	*st = rd32(d + 28);
	*da = rd32(d + 32);
	if (*tt >= len || *st > len || *st < *tt || *da < *st || *da > len)
		return 0;
	*sides = (int)((*st - *tt) / 4);
	return 1;
}

/* Whether a side, as the image gives it, is the disc's regular layout or a part of it: sectors of
   one size, each ID once and under the count, with no CRC errors */
static int regular(const dside_t *side, int count, uint32_t size)
{
	uint8_t seen[256] = { 0 };
	int k;

	for (k = 0; k < side->n; k++)
	{
		const dsector_t *s = &side->s[k];

		if (s->len != size || s->r >= count || seen[s->r] || s->id_crc || s->data_crc)
			return 0;
		seen[s->r] = 1;
	}
	return 1;
}

static int by_int(const void *a, const void *b)
{
	return *(const int *)a - *(const int *)b;
}

/* A JFD leaves out the sides and sectors that held nothing. On the disc they were formatted like
   the rest, so up to track 79 they are put back, in the commonest regular layout, full of zeros.
   Where the image gives the regular sides' sectors places on the track, a sector put back goes
   where that sector most often is. */
static void jfd_fill(disc_t *disc)
{
	int votes[64][8] = { { 0 } }, best = 0, count = 0, code = 0, density = DENS_DOUBLE, c, h, k;
	int place[64], *seen, n_seen;

	for (c = 0; c < DISC_CYLS; c++)
		for (h = 0; h < 2; h++)
		{
			const dside_t *side = &disc->side[c][h];
			int top = 0, cd;

			if (!side->n)
				continue;
			for (k = 0; k < side->n; k++)
				if (side->s[k].r + 1 > top)
					top = side->s[k].r + 1;
			cd = side->s[0].n & 7;
			if (top <= 64 && regular(side, top, 128u << cd) && ++votes[top - 1][cd] > best)
			{
				best = votes[top - 1][cd];
				count = top;
				code = cd;
				density = side->s[0].density;
			}
		}
	if (!best)
		return;
	/* Where each sector of the regular layout most often starts, or -1 for nowhere in particular */
	seen = malloc(DISC_CYLS * 2 * sizeof *seen);
	for (k = 0; k < count; k++)
	{
		int i, run = 0, top = 0;

		n_seen = 0;
		for (c = 0; c < DISC_CYLS; c++)
			for (h = 0; h < 2; h++)
			{
				const dside_t *side = &disc->side[c][h];
				int j;

				if (!side->n || !regular(side, count, 128u << code))
					continue;
				for (j = 0; j < side->n; j++)
					if (side->s[j].r == k)
						seen[n_seen++] = side->s[j].pos;
			}
		qsort(seen, n_seen, sizeof *seen, by_int);
		place[k] = -1;
		for (i = 0; i < n_seen; i++)
		{
			run = i && seen[i] == seen[i - 1] ? run + 1 : 1;
			if (run > top)
			{
				top = run;
				place[k] = seen[i];
			}
		}
	}
	free(seen);
	for (c = 0; c < 80; c++)
		for (h = 0; h < 2; h++)
		{
			dside_t *side = &disc->side[c][h];
			dsector_t *s;

			if (side->n >= count || !regular(side, count, 128u << code))
				continue;
			s = calloc(count, sizeof *s);
			for (k = 0; k < count; k++)
			{
				int j;

				for (j = 0; j < side->n && side->s[j].r != k; j++);
				if (j < side->n)
					s[k] = side->s[j];
				else
				{
					s[k].c = (uint8_t)c;
					s[k].h = (uint8_t)h;
					s[k].r = (uint8_t)k;
					s[k].n = (uint8_t)code;
					s[k].density = (uint8_t)density;
					s[k].pos = place[k];
					s[k].len = 128u << code;
					s[k].data = calloc(1, s[k].len);
				}
			}
			free(side->s);
			side->s = s;
			side->n = count;
			side->bytes = density == DENS_HIGH ? DISC_HD_BYTES : DISC_DD_BYTES;
		}
	if (disc->cyls < 80)
		disc->cyls = 80;
}

static int jfd_parse(disc_t *disc, const uint8_t *d, size_t len)
{
	uint32_t tt, st, da;
	int sides, i;

	if (!jfd_tables(d, len, &tt, &st, &da, &sides))
		return -1;
	for (i = 0; i < sides; i++)
	{
		uint32_t off = rd32(d + tt + 4 * i), p, bytes = 0;
		int cyl = i / 2, head = i & 1, n = 0, k, high = 0;
		dside_t *side;

		if (off == 0xffffffff || cyl >= DISC_CYLS)
			continue;
		/* A side's list of sectors is in the sector table, ending with &FFFFFFFF, and each sector's
		   data is in the file. JASPP's sides hold 19 sectors and 16K of data at most, so a side of
		   more than 255 sectors or 64K is damaged, and so is the image. */
		if (off >= da - st)
			return -1;
		for (p = st + off; p + 4 <= da && rd32(d + p) != 0xffffffff; p += 8)
		{
			uint32_t w1 = rd32(d + p), w2 = p + 8 <= da ? rd32(d + p + 4) : 0, size = 128u << ((w1 & 15) > 7 ? 7 : (w1 & 15));

			bytes += size;
			if (p + 8 > da || ++n > 255 || bytes > 65536 || (w2 != 0xffffffff && (uint64_t)da + w2 + size > len))
				return -1;
		}
		if (p + 4 > da)
			return -1;
		if (!n)
			continue;
		side = &disc->side[cyl][head];
		side->s = calloc(n, sizeof(dsector_t));
		side->n = n;
		for (k = 0, p = st + off; k < n; k++, p += 8)
		{
			uint32_t w1 = rd32(d + p), w2 = rd32(d + p + 4);
			dsector_t *s = &side->s[k];
			int code = w1 & 15, dens = w1 >> 16 & 15;

			if (code > 7)
				code = 7;
			s->c = (uint8_t)cyl;
			s->h = (uint8_t)head;
			s->r = (uint8_t)(w1 >> 8);
			s->n = (uint8_t)code;
			s->id_crc = w1 >> 4 & 1;
			s->data_crc = w1 >> 5 & 1;
			s->density = dens == 4 ? DENS_HIGH : dens == 1 ? DENS_SINGLE : DENS_DOUBLE;
			high |= s->density == DENS_HIGH;
			s->len = 128u << code;
			s->data = calloc(1, s->len);
			if (w2 != 0xffffffff)
				memcpy(s->data, d + da + w2, s->len);
			s->pos = (int)(w1 >> 24);       /* milliseconds for now, or 255 for anywhere */
		}
		side->bytes = high ? DISC_HD_BYTES : DISC_DD_BYTES;
		for (k = 0; k < n; k++)
			side->s[k].pos = side->s[k].pos == 255 ? -1 : side->s[k].pos * side->bytes / 200 + 15;
		if (cyl + 1 > disc->cyls)
			disc->cyls = cyl + 1;
	}
	jfd_fill(disc);
	for (i = 0; i < DISC_CYLS * 2; i++)
		disc_layout(&disc->side[i / 2][i & 1]);
	return 0;
}

/* ---- HFE ---- */

typedef struct
{
	const uint8_t *d;
	size_t len;
	uint32_t off;           /* the track's first block */
	int side;
	uint32_t cells;         /* MFM cells in the side's revolution */
} cells_t;

/* One cell of a side's stream. Each 512 byte block holds 256 bytes of side 0 and then 256 of
   side 1, and the cells go from the low bit of each byte up. */
static int cell(const cells_t *c, uint32_t k)
{
	uint32_t i, src;

	k %= c->cells;
	i = k >> 3;
	src = c->off + (i / 256) * 512 + c->side * 256 + i % 256;
	return src < c->len ? c->d[src] >> (k & 7) & 1 : 0;
}

static unsigned raw16(const cells_t *c, uint32_t k)
{
	unsigned v = 0;
	int b;

	for (b = 0; b < 16; b++)
		v = v << 1 | cell(c, k + b);
	return v;
}

/* The byte whose 16 cells start at k: its data bits are the second of each pair */
static uint8_t mfm_byte(const cells_t *c, uint32_t k)
{
	unsigned v = 0;
	int b;

	for (b = 0; b < 8; b++)
		v = v << 1 | cell(c, k + 2 * b + 1);
	return (uint8_t)v;
}

static void hfe_side(disc_t *disc, const cells_t *c, int cyl, int head, int bytes, int density)
{
	dside_t *side = &disc->side[cyl][head];
	dsector_t *s = NULL;
	int n = 0, last = -1;
	uint32_t k, last_cell = 0;
	unsigned sr = 0;

	for (k = 0; k < c->cells; k++)
	{
		uint32_t j;
		uint8_t mark, b[8];
		int i;

		sr = (sr << 1 | cell(c, k)) & 0xffff;
		if (sr != 0x4489)
			continue;
		for (j = k + 1; raw16(c, j) == 0x4489; j += 16);
		mark = mfm_byte(c, j);
		if (mark == 0xfe)
		{
			s = realloc(s, (n + 1) * sizeof *s);
			memset(&s[n], 0, sizeof *s);
			for (i = 0; i < 6; i++)
				b[i] = mfm_byte(c, j + 16 + 16 * i);
			s[n].c = b[0];
			s[n].h = b[1];
			s[n].r = b[2];
			s[n].n = b[3];
			s[n].id_crc = disc_id_crc(&s[n]) != (b[4] << 8 | b[5]);
			s[n].density = (uint8_t)density;
			s[n].pos = (int)((uint64_t)(j % c->cells) * bytes / c->cells);
			s[n].len = 128u << (b[3] & 3);
			s[n].data = calloc(1, s[n].len);
			s[n].data_crc = 1;              /* until a data field turns up */
			last = n++;
			last_cell = j;
			k = j + 16 * 7 - 1;
			sr = 0;
		}
		else if ((mark == 0xfb || mark == 0xf8 || mark == 0xf9 || mark == 0xfa) && last >= 0 && j - last_cell < 16 * 64)
		{
			dsector_t *t = &s[last];
			uint32_t at = j + 16;

			for (i = 0; i < (int)t->len; i++, at += 16)
				t->data[i] = mfm_byte(c, at);
			b[0] = mfm_byte(c, at);
			b[1] = mfm_byte(c, at + 16);
			t->data_crc = disc_data_crc(t->data, t->len, mark) != (b[0] << 8 | b[1]);
			last = -1;
			k = at + 32 - 1;
			sr = 0;
		}
	}
	side->s = s;
	side->n = n;
	side->bytes = bytes;
	disc_layout(side);
	if (n && cyl + 1 > disc->cyls)
		disc->cyls = cyl + 1;
}

static int hfe_parse(disc_t *disc, const uint8_t *d, size_t len)
{
	int tracks, sides, t, h, high;
	uint32_t list;

	if (len < 512 || memcmp(d, "HXCPICFE", 8))
		return -1;
	tracks = d[9];
	sides = d[10] > 2 ? 2 : d[10];
	high = rd16(d + 12) >= 400;
	list = rd16(d + 18) * 512;
	for (t = 0; t < tracks && t < DISC_CYLS; t++)
	{
		const uint8_t *e;
		cells_t c;

		if (list + 4 * t + 4 > len)
			break;
		e = d + list + 4 * t;
		c.d = d;
		c.len = len;
		c.off = rd16(e) * 512;
		c.cells = rd16(e + 2) / 2 * 8;
		if (!c.cells)
			continue;
		for (h = 0; h < sides; h++)
		{
			c.side = h;
			hfe_side(disc, &c, t, h, high ? DISC_HD_BYTES : DISC_DD_BYTES, high ? DENS_HIGH : DENS_DOUBLE);
		}
	}
	return 0;
}

/* ---- Flat images ---- */

typedef struct
{
	uint32_t size;
	int cyls, heads, sectors, bytes, first, high;
} shape_t;

static const shape_t shapes[] = {
	{ 819200, 80, 2, 5, 1024, 0, 0 },       /* ADFS D and E, 800K */
	{ 655360, 80, 2, 16, 256, 0, 0 },       /* ADFS L, 640K (ADL) */
	{ 1638400, 80, 2, 10, 1024, 0, 1 },     /* ADFS F, 1.6MB high density */
	{ 327680, 80, 1, 16, 256, 0, 0 },       /* ADFS M, 320K */
	{ 163840, 40, 1, 16, 256, 0, 0 },       /* ADFS S, 160K */
	{ 737280, 80, 2, 9, 512, 1, 0 },        /* DOS 720K */
	{ 368640, 40, 2, 9, 512, 1, 0 },        /* DOS 360K */
	{ 1474560, 80, 2, 18, 512, 1, 1 },      /* DOS 1.44MB */
};

static int flat_parse(disc_t *disc, const uint8_t *d, size_t len)
{
	const shape_t *sh = &shapes[0];
	shape_t longer = shapes[0];
	size_t i, at = 0;
	int c, h, k, code;

	for (i = 0; i < sizeof shapes / sizeof *shapes; i++)
		if (len == shapes[i].size)
			sh = &shapes[i];
	if (sh == &shapes[0] && len > shapes[0].size && len <= shapes[0].size + 4 * 10240 && len % 10240 == 0)
	{
		longer.cyls = (int)(len / 10240);       /* an 800K format with up to 4 tracks more, which FileCore allows */
		sh = &longer;
	}
	else if (sh == &shapes[0] && len > shapes[0].size)
		sh = &shapes[2];        /* bigger than 800K: lay it out as a 1.6MB disc */
	else if (sh == &shapes[0] && len && len < shapes[0].size && len % 10240 == 0)
	{
		/* An 800K image that stops at the end of a cylinder holds the cylinders the disc had
		   formatted, and the ones after it are left unformatted, as on a disc duplicated with
		   only the tracks it uses. STARCH's protection checks that cylinder 64 can't be read. */
		longer.cyls = (int)(len / 10240);
		sh = &longer;
	}
	for (code = 0; (128 << code) < sh->bytes; code++);
	for (c = 0; c < sh->cyls; c++)
		for (h = 0; h < sh->heads; h++)
		{
			dside_t *side = &disc->side[c][h];

			side->s = calloc(sh->sectors, sizeof(dsector_t));
			side->n = sh->sectors;
			side->bytes = sh->high ? DISC_HD_BYTES : DISC_DD_BYTES;
			for (k = 0; k < sh->sectors; k++)
			{
				dsector_t *s = &side->s[k];

				s->c = (uint8_t)c;
				s->h = (uint8_t)h;
				s->r = (uint8_t)(k + sh->first);
				s->n = (uint8_t)code;
				s->density = sh->high ? DENS_HIGH : DENS_DOUBLE;
				s->pos = -1;
				s->len = sh->bytes;
				s->data = calloc(1, s->len);
				if (at < len)
					memcpy(s->data, d + at, len - at < s->len ? len - at : s->len);
				at += s->len;
			}
			disc_layout(side);
		}
	disc->cyls = sh->cyls;
	return 0;
}

disc_t *disc_parse(const uint8_t *d, size_t len)
{
	disc_t *disc = calloc(1, sizeof *disc);
	int r;

	if (len >= 4 && !memcmp(d, "JFDI", 4))
		r = is_jfd(d, len) ? jfd_parse(disc, d, len) : -1;     /* too short for a JFD's header */
	else if (len >= 8 && !memcmp(d, "HXCPICFE", 8))
		r = hfe_parse(disc, d, len);
	else
		r = flat_parse(disc, d, len);
	if (r)
	{
		disc_free(disc);
		return NULL;
	}
	return disc;
}

/* ---- Saved changes ---- */

static void sav_path(const disc_t *d, char *buf, size_t size)
{
	snprintf(buf, size, "%s.sav", d->path);
}

/* Puts the sides kept in a .sav file's bytes over the disc. Bytes that aren't one of these are left be. */
void disc_apply(disc_t *disc, const uint8_t *d, size_t len)
{
	size_t at = 16;

	if (!d || len < 16 || memcmp(d, SAV_MAGIC, 16))
		return;
	while (at + 12 <= len && !memcmp(d + at, "SIDE", 4))
	{
		int cyl = d[at + 4], head = d[at + 5], n = (int)rd16(d + at + 6), bytes = (int)rd32(d + at + 8), k, ok = 1;
		dsector_t *s = calloc(n ? n : 1, sizeof *s);

		at += 12;
		for (k = 0; k < n; k++)
		{
			if (at + 16 > len)
			{
				ok = 0;
				break;
			}
			s[k].c = d[at];
			s[k].h = d[at + 1];
			s[k].r = d[at + 2];
			s[k].n = d[at + 3];
			s[k].id_crc = d[at + 4] & 1;
			s[k].data_crc = d[at + 4] >> 1 & 1;
			s[k].density = d[at + 5];
			s[k].pos = (int)rd32(d + at + 8);
			s[k].len = rd32(d + at + 12);
			at += 16;
			if (s[k].len > 65536 || at + s[k].len > len)
			{
				ok = 0;
				break;
			}
			s[k].data = malloc(s[k].len ? s[k].len : 1);
			memcpy(s[k].data, d + at, s[k].len);
			at += s[k].len;
		}
		if (!ok || cyl >= DISC_CYLS || head > 1)
		{
			while (k-- > 0)
				free(s[k].data);
			free(s);
			break;
		}
		disc_set_side(disc, cyl, head, s, n, bytes);
	}
}

static void sav_load(disc_t *disc)
{
	char path[1100];
	size_t len;
	uint8_t *d;

	sav_path(disc, path, sizeof path);
	d = file_read(path, &len);
	disc_apply(disc, d, len);
	free(d);
}

/* What has been written to the disc since its image was made, as the bytes of a .sav file, or
   NULL if nothing has */
uint8_t *disc_changes(const disc_t *disc, size_t *len)
{
	size_t size = 16, at;
	uint8_t *out;
	int c, h, k;

	*len = 0;
	if (!disc || !disc->changed)
		return NULL;
	for (c = 0; c < DISC_CYLS; c++)
		for (h = 0; h < 2; h++)
			if (disc->side[c][h].changed)
			{
				size += 12;
				for (k = 0; k < disc->side[c][h].n; k++)
					size += 16 + disc->side[c][h].s[k].len;
			}
	out = malloc(size);
	memcpy(out, SAV_MAGIC, 16);
	at = 16;
	for (c = 0; c < DISC_CYLS; c++)
		for (h = 0; h < 2; h++)
		{
			const dside_t *side = &disc->side[c][h];

			if (!side->changed)
				continue;
			memcpy(out + at, "SIDE", 4);
			out[at + 4] = (uint8_t)c;
			out[at + 5] = (uint8_t)h;
			wr16(out + at + 6, side->n);
			wr32(out + at + 8, side->bytes);
			at += 12;
			for (k = 0; k < side->n; k++)
			{
				const dsector_t *s = &side->s[k];

				out[at] = s->c;
				out[at + 1] = s->h;
				out[at + 2] = s->r;
				out[at + 3] = s->n;
				out[at + 4] = (uint8_t)(s->id_crc | s->data_crc << 1);
				out[at + 5] = s->density;
				out[at + 6] = out[at + 7] = 0;
				wr32(out + at + 8, (uint32_t)s->pos);
				wr32(out + at + 12, s->len);
				memcpy(out + at + 16, s->data, s->len);
				at += 16 + s->len;
			}
		}
	*len = at;
	return out;
}

int disc_save(disc_t *disc)
{
	char path[1100];
	size_t len;
	uint8_t *out;
	int r;

	if (!disc || !disc->changed || !disc->path[0])
		return 0;
	out = disc_changes(disc, &len);
	sav_path(disc, path, sizeof path);
	r = file_write(path, out, len);
	free(out);
	return r;
}

/* The image as it was made, with no saved changes */
disc_t *disc_open_plain(const char *path)
{
	size_t len;
	uint8_t *d = jfd_read(path, &len);
	disc_t *disc;

	if (!d)
		return NULL;
	disc = disc_parse(d, len);
	free(d);
	if (disc)
		snprintf(disc->path, sizeof disc->path, "%s", path);
	return disc;
}

disc_t *disc_open(const char *path)
{
	disc_t *disc = disc_open_plain(path);

	if (disc)
		sav_load(disc);
	return disc;
}

/* ---- The track as a Read Track gives it ---- */

static void put(uint8_t *out, uint8_t *sync, int bytes, int at, uint8_t v, int is_sync)
{
	at %= bytes;
	if (at < 0)
		at += bytes;
	out[at] = v;
	if (sync)
		sync[at] = (uint8_t)is_sync;
}

int disc_track_bytes(const dside_t *side, uint8_t *out, uint8_t *sync)
{
	int bytes = side->bytes ? side->bytes : DISC_DD_BYTES, i, k, at;

	memset(out, 0x4e, bytes);
	if (sync)
		memset(sync, 0, bytes);
	for (i = 0; i < side->n; i++)
	{
		const dsector_t *s = &side->s[i];
		uint16_t crc = disc_id_crc(s);
		uint8_t id[6] = { s->c, s->h, s->r, s->n, (uint8_t)(crc >> 8), (uint8_t)crc };

		if (s->id_crc)
			id[5] ^= 0x5a;
		at = s->pos - 15;
		for (k = 0; k < 12; k++)
			put(out, sync, bytes, at++, 0, 0);
		for (k = 0; k < 3; k++)
			put(out, sync, bytes, at++, 0xa1, 1);
		put(out, sync, bytes, at++, 0xfe, 0);
		for (k = 0; k < 6; k++)
			put(out, sync, bytes, at++, id[k], 0);
		at += 22;
		for (k = 0; k < 12; k++)
			put(out, sync, bytes, at++, 0, 0);
		for (k = 0; k < 3; k++)
			put(out, sync, bytes, at++, 0xa1, 1);
		put(out, sync, bytes, at++, 0xfb, 0);
		for (k = 0; k < (int)s->len; k++)
			put(out, sync, bytes, at++, s->data[k], 0);
		crc = disc_data_crc(s->data, s->len, 0xfb) ^ (s->data_crc ? 0x5a5a : 0);
		put(out, sync, bytes, at++, (uint8_t)(crc >> 8), 0);
		put(out, sync, bytes, at++, (uint8_t)crc, 0);
	}
	return bytes;
}

/* ---- Flattening a JFD for reading the files on it ---- */

/* Whether a side holds sectors of one size, each ID once and under the given count */
static int fits(const dside_t *side, int count, uint32_t size)
{
	uint8_t seen[256] = { 0 };
	int k;

	for (k = 0; k < side->n; k++)
	{
		if (side->s[k].len != size || side->s[k].r >= count || seen[side->s[k].r])
			return 0;
		seen[side->s[k].r] = 1;
	}
	return 1;
}

static int by_id(const void *a, const void *b)
{
	return (*(const dsector_t *const *)a)->r - (*(const dsector_t *const *)b)->r;
}

/* A disc's sides as one run of sectors, which frees the disc. A side that doesn't have the regular
   layout goes in as its sectors in ID order, or with geometric, in the room a regular side takes,
   each of its sectors of the regular size and an ID under the count where ADFS would look for it. */
static uint8_t *lay_out(disc_t *disc, int geometric, size_t *len)
{
	int i, k, count = 5, last = -1, votes[64][8] = { { 0 } }, best = 0, nout;
	uint32_t size = 1024;
	uint8_t *out;
	size_t at = 0, cap;

	/* The regular layout: the commonest count and size among sides with one size and IDs from 0.
	   The sides the image left out up to track 79 are back in by now, so an 800K disc comes out
	   as 800K. */
	for (i = 0; i < DISC_CYLS * 2; i++)
	{
		const dside_t *side = &disc->side[i / 2][i & 1];
		int top = 0, code;

		if (!side->n)
			continue;
		last = i;
		for (k = 0; k < side->n; k++)
			if (side->s[k].r + 1 > top)
				top = side->s[k].r + 1;
		code = side->s[0].n & 7;
		if (top <= 64 && fits(side, top, side->s[0].len) && ++votes[top - 1][code] > best)
		{
			best = votes[top - 1][code];
			count = top;
			size = 128u << code;
		}
	}
	nout = last + 1;
	cap = (size_t)nout * count * size + 65536;
	out = calloc(1, cap);
	for (i = 0; i < nout; i++)
	{
		const dside_t *side = &disc->side[i / 2][i & 1];

		if (fits(side, count, size))
		{
			for (k = 0; k < side->n; k++)
				memcpy(out + at + side->s[k].r * size, side->s[k].data, size);
			at += (size_t)count * size;
		}
		else if (geometric)
		{
			for (k = 0; k < side->n; k++)
				if (side->s[k].r < count && side->s[k].len == size)
					memcpy(out + at + side->s[k].r * size, side->s[k].data, size);
			at += (size_t)count * size;
		}
		else
		{
			const dsector_t **order = malloc(side->n * sizeof *order);

			for (k = 0; k < side->n; k++)
				order[k] = &side->s[k];
			qsort(order, side->n, sizeof *order, by_id);
			for (k = 0; k < side->n; k++)
			{
				if (at + order[k]->len > cap)
				{
					cap = cap * 2 + order[k]->len;
					out = realloc(out, cap);
				}
				memcpy(out + at, order[k]->data, order[k]->len);
				at += order[k]->len;
			}
			free(order);
		}
	}
	disc_free(disc);
	*len = at;
	return out;
}

static uint8_t *flatten(const uint8_t *raw, size_t raw_len, size_t *len)
{
	uint32_t tt, st, da;
	int sides;
	disc_t *disc;

	if (!jfd_tables(raw, raw_len, &tt, &st, &da, &sides) || !(disc = disc_parse(raw, raw_len)))
		return NULL;
	return lay_out(disc, 0, len);
}

uint8_t *disc_sectors(const char *path, size_t *len)
{
	disc_t *disc = disc_open(path);

	return disc ? lay_out(disc, 1, len) : NULL;
}

uint8_t *jfd_flatten(const char *path, size_t *len)
{
	size_t n;
	uint8_t *d = jfd_read(path, &n), *flat;

	if (!d)
		return NULL;
	if (!is_jfd(d, n))
	{
		*len = n;
		return d;
	}
	flat = flatten(d, n, len);
	free(d);
	return flat;
}

/* ---- The self-test ---- */

/* An MFM encoding of a side's bytes, one cell a bit from the low bit of each byte up, as HFE has it */
static void mfm_encode(const uint8_t *bytes, const uint8_t *sync, int n, uint8_t *cells)
{
	int i, b, prev = 0;
	uint32_t k = 0;

	memset(cells, 0, (size_t)n * 2);
	for (i = 0; i < n; i++)
	{
		unsigned v = 0;

		if (sync[i])
			v = 0x4489;
		else
			for (b = 7; b >= 0; b--)
			{
				int bit = bytes[i] >> b & 1;

				v = v << 2 | (!prev && !bit) << 1 | bit;
				prev = bit;
			}
		if (sync[i])
			prev = 1;
		for (b = 15; b >= 0; b--, k++)
			if (v >> b & 1)
				cells[k >> 3] |= (uint8_t)(1 << (k & 7));
	}
}

/* A gzip file made from data in memory, as JASPP's images are */
static uint8_t *gzip(const uint8_t *d, size_t len, size_t *out_len)
{
	uLongf zlen = compressBound((uLong)len);
	uint8_t *z = malloc(zlen), *out;
	uLong crc;

	if (compress2(z, &zlen, d, (uLong)len, 6) != Z_OK)
	{
		free(z);
		return NULL;
	}
	/* zlib's two byte header and four byte trailer go; the deflate data in between stays */
	out = malloc(zlen - 6 + 18);
	memcpy(out, "\x1f\x8b\x08\x00\x00\x00\x00\x00\x00\x03", 10);
	memcpy(out + 10, z + 2, zlen - 6);
	crc = crc32(0, d, (uInt)len);
	wr32(out + 10 + zlen - 6, (uint32_t)crc);
	wr32(out + 14 + zlen - 6, (uint32_t)len);
	*out_len = zlen - 6 + 18;
	free(z);
	return out;
}

/* Builds a small JFD: track 0 side 0 a regular 800K layout, track 1 side 0 the kinds of sector
   copy protection uses, and checks that it reads back, flattens, and survives being written as
   an HFE and decoded again. Returns 0 when it all holds, or the number of the check that failed. */
int disc_selftest(void)
{
	static const uint32_t prot[3][2] = {
		{ 0x0a000900 | 2 | 1 << 5 | 2 << 16, 5 * 1024 },        /* ID 9, 512 bytes, data CRC error, at 10 ms */
		{ 0x3200c800 | 1 | 1 << 4 | 2 << 16, 5 * 1024 + 512 },  /* ID 200, 256 bytes, ID CRC error, at 50 ms */
		{ 0x78000300 | 0 | 2 << 16, 0xffffffff },               /* ID 3, 128 bytes of nothing, at 120 ms */
	};
	uint8_t *jfd = calloc(1, 8192 + 6 * 1024), *gz, *back, *flat, *hfe, *track, *sync;
	size_t gz_len, back_len, flat_len;
	disc_t *disc, *again;
	int i, k, fail = 0;
	uint32_t at;

	/* 1: the CRC, against its standard check value */
	if (disc_crc(0xffff, (const uint8_t *)"123456789", 9) != 0x29b1)
		return 1;

	/* The JFD: 4 sides, of which 1 and 3 are empty */
	memcpy(jfd, "JFDI", 4);
	wr32(jfd + 24, 512);
	wr32(jfd + 28, 512 + 16);
	wr32(jfd + 32, 1024);
	wr32(jfd + 512, 0);
	wr32(jfd + 516, 0xffffffff);
	wr32(jfd + 520, 5 * 8 + 4);
	wr32(jfd + 524, 0xffffffff);
	at = 528;
	for (i = 0; i < 5; i++, at += 8)
	{
		wr32(jfd + at, 3 | i << 8 | 2 << 16 | 0xffu << 24);
		wr32(jfd + at + 4, i * 1024);
	}
	wr32(jfd + at, 0xffffffff);
	at += 4;
	for (i = 0; i < 3; i++, at += 8)
	{
		wr32(jfd + at, prot[i][0]);
		wr32(jfd + at + 4, prot[i][1]);
	}
	wr32(jfd + at, 0xffffffff);
	for (i = 0; i < 6 * 1024; i++)
		jfd[1024 + i] = (uint8_t)(i * 7 + i / 1024);

	/* 2: it comes through gzip, packed as two members one after the other */
	{
		size_t len1, len2;
		uint8_t *gz1 = gzip(jfd, 316, &len1), *gz2 = gzip(jfd + 316, 8192 - 316, &len2);

		gz = malloc(len1 + len2);
		memcpy(gz, gz1, len1);
		memcpy(gz + len1, gz2, len2);
		gz_len = len1 + len2;
		free(gz1);
		free(gz2);
	}
	back = gunzip(gz, gz_len, &back_len);
	if (!back || back_len != 8192 || memcmp(back, jfd, 8192))
		fail = 2;
	free(gz);
	free(back);

	/* 3: the sides read as they were written, and the empty ones as formatted sides of zeros */
	disc = disc_parse(jfd, 8192);
	if (!fail && (!disc || disc->side[0][0].n != 5 || disc->side[0][1].n != 5 || disc->side[0][1].s[4].data[1023] ||
		      disc->side[1][0].n != 3 || disc->side[79][1].n != 5 || disc->side[80][0].n))
		fail = 3;
	for (i = 0; !fail && i < 5; i++)
		if (disc->side[0][0].s[i].r != i || disc->side[0][0].s[i].len != 1024 || memcmp(disc->side[0][0].s[i].data, jfd + 1024 + i * 1024, 1024))
			fail = 3;
	/* 4: the protected side in track order, with its errors and sizes */
	if (!fail)
	{
		const dsector_t *s = disc->side[1][0].s;

		if (s[0].r != 9 || s[1].r != 200 || s[2].r != 3 || s[0].len != 512 || s[1].len != 256 || s[2].len != 128 ||
		    !s[0].data_crc || s[0].id_crc || !s[1].id_crc || s[1].data_crc || s[2].data[0] || s[1].data[0] != jfd[1024 + 5 * 1024 + 512] ||
		    s[0].pos != 10 * DISC_DD_BYTES / 200 + 15 || s[2].pos != 120 * DISC_DD_BYTES / 200 + 15)
			fail = 4;
	}
	/* 5: flattened, 80 tracks: the regular sides by ID and the empty ones as zeros, the other in ID order */
	flat = disc && !fail ? flatten(jfd, 8192, &flat_len) : NULL;
	if (!fail && (!flat || flat_len != 159 * 5 * 1024 + 128 + 512 + 256 || memcmp(flat, jfd + 1024, 5 * 1024) ||
		      flat[5 * 1024] || flat[10 * 1024] || memcmp(flat + 10 * 1024 + 128, jfd + 1024 + 5 * 1024, 512)))
		fail = 5;
	free(flat);

	/* 6: written out as an HFE of 2 tracks and read back, each sector the same */
	if (!fail)
	{
		int blocks = (DISC_DD_BYTES * 2 + 255) / 256;

		hfe = calloc(1, 1024 + 2 * blocks * 512);
		track = malloc(DISC_DD_BYTES);
		sync = malloc(DISC_DD_BYTES);
		memset(hfe, 0xff, 512);
		memcpy(hfe, "HXCPICFE", 8);
		hfe[8] = 0;
		hfe[9] = 2;
		hfe[10] = 2;
		hfe[11] = 0;
		wr16(hfe + 12, 250);
		wr16(hfe + 14, 300);
		hfe[16] = 7;
		wr16(hfe + 18, 1);
		for (i = 0; i < 2; i++)
		{
			wr16(hfe + 512 + 4 * i, 2 + i * blocks);
			wr16(hfe + 514 + 4 * i, DISC_DD_BYTES * 2 * 2);
			for (k = 0; k < 2; k++)
			{
				uint8_t *cells = calloc(1, DISC_DD_BYTES * 2);
				int b;

				disc_track_bytes(&disc->side[i][k], track, sync);
				mfm_encode(track, sync, DISC_DD_BYTES, cells);
				for (b = 0; b < DISC_DD_BYTES * 2; b++)
					hfe[(2 + i * blocks) * 512 + (b / 256) * 512 + k * 256 + b % 256] = cells[b];
				free(cells);
			}
		}
		again = disc_parse(hfe, 1024 + 2 * blocks * 512);
		for (i = 0; i < 4 && !fail; i++)
		{
			const dside_t *a = &disc->side[i / 2][i & 1], *b = again ? &again->side[i / 2][i & 1] : NULL;

			if (!b || a->n != b->n)
				fail = 6;
			for (k = 0; !fail && k < a->n; k++)
				if (a->s[k].r != b->s[k].r || a->s[k].len != b->s[k].len || a->s[k].id_crc != b->s[k].id_crc ||
				    a->s[k].data_crc != b->s[k].data_crc || abs(a->s[k].pos - b->s[k].pos) > 1 || memcmp(a->s[k].data, b->s[k].data, a->s[k].len))
					fail = 6;
		}
		disc_free(again);
		free(hfe);
		free(track);
		free(sync);
	}
	disc_free(disc);
	free(jfd);
	return fail;
}
