/* PiArcBoot: Spark and ArcFS archives, RISC OS's own archive formats, read into the list of files
   a zip gives (adfs.c), for a game that comes as applications in one.

   A Spark archive is ARC's format with RISC OS's file information added. Each entry is &1A, a
   method byte with bit 7 set, a name of up to 12 characters in 13 bytes, the compressed length,
   an MS-DOS date and time, the CRC-16 of the file, its length (for methods above 1), then its load
   and execution addresses and attributes, and the data. A directory is a stored entry of type &DDC
   whose entries follow it, up to an entry of method 0. Method 0 at the top ends the archive. An
   ARC archive from MS-DOS is the same without the RISC OS part.

   An ArcFS archive starts "Archive", a NUL, the length of its table of entries, where the data
   starts, the versions that wrote it and can read it and a format number, 96 bytes in all, then
   the table. Each entry is 36 bytes: an object byte (0 the end of a directory, 1 deleted, &82
   stored, &83 packed, &88 crunched, &FF compressed), an 11 character name, the length, the load
   and execution addresses, a word holding the attributes in its low byte, the largest code size
   in the next and the CRC-16 in its top half, the compressed length, and a word with bit 31 set
   for a directory and otherwise the data's place after the start of the data. A directory's
   entries follow it, up to an end of directory entry.

   The methods. Stored is the file. Packed is runs: &90 and a count n stands for the byte before
   it n times in all, and &90 0 for &90 itself. Crunched is LZW, codes of 9 bits up to the largest
   size, code 256 clearing the table, and then unpacked as packed is. Compressed is the same LZW
   with no runs, and squashed LZW of up to 13 bits. A Spark archive's crunched or compressed file
   starts with its largest code size, and an ArcFS archive keeps it in the entry. The codes are
   read as Unix compress wrote them, low bit first, a code size's worth of bytes at a time, with
   the rest of a group thrown away when the code size grows or the table is cleared. Each file's
   CRC-16, ARC's (polynomial &A001), is checked. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "piarcboot.h"

#define MAX_DEPTH 32

static uint32_t rd32(const uint8_t *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

/* ---- What a method gives ---- */

typedef struct
{
	uint8_t *buf;
	size_t len, at;
	int running;            /* the byte before was &90 */
	uint8_t prev;
	uint16_t crc;
} out_t;

static uint16_t crc_table[256];

static void put(out_t *o, uint8_t b)
{
	if (o->at < o->len)
	{
		o->buf[o->at++] = b;
		o->crc = (uint16_t)(o->crc >> 8 ^ crc_table[(o->crc ^ b) & 0xFF]);
	}
}

static void put_unpacked(out_t *o, uint8_t b)
{
	if (o->running)
	{
		if (!b)
			put(o, 0x90);
		else
			while (--b)
				put(o, o->prev);
		o->running = 0;
	}
	else if (b == 0x90)
		o->running = 1;
	else
	{
		o->prev = b;
		put(o, b);
	}
}

/* LZW from in (len bytes), each byte of the result through put_unpacked if runs is set */
static int lzw(const uint8_t *in, size_t len, int maxbits, int runs, out_t *o)
{
	static uint16_t prefix[65536];
	static uint8_t suffix[65536], stack[65536];
	uint8_t group[16];
	size_t size = 0, offset = 0;
	int n_bits = 9, clear = 0, sp, code, oldcode, incode, finchar, i;
	long maxcode = 511, maxmaxcode = (1L << maxbits) - 1, free_ent = 257;

	if (maxbits < 9 || maxbits > 16)
		return -1;
	for (i = 0; i < 256; i++)
	{
		prefix[i] = 0;
		suffix[i] = (uint8_t)i;
	}
#define NEXT_CODE(c) \
	do { \
		if (clear || offset >= size || free_ent > maxcode) \
		{ \
			size_t take; \
			if (free_ent > maxcode) \
			{ \
				n_bits++; \
				maxcode = n_bits == maxbits ? maxmaxcode : (1L << n_bits) - 1; \
			} \
			if (clear) \
			{ \
				n_bits = 9; \
				maxcode = 511; \
				clear = 0; \
			} \
			take = len < (size_t)n_bits ? len : (size_t)n_bits; \
			if (!take) \
			{ \
				(c) = -1; \
				break; \
			} \
			memcpy(group, in, take); \
			in += take; \
			len -= take; \
			offset = 0; \
			size = take * 8 - (size_t)(n_bits - 1); \
		} \
		{ \
			size_t byte = offset >> 3, bit = offset & 7; \
			uint32_t v = group[byte] | (byte + 1 < 16 ? group[byte + 1] << 8 : 0) | (byte + 2 < 16 ? (uint32_t)group[byte + 2] << 16 : 0); \
			(c) = (int)(v >> bit & ((1u << n_bits) - 1)); \
			offset += (size_t)n_bits; \
		} \
	} while (0)

	memset(group, 0, sizeof group);
	NEXT_CODE(oldcode);
	if (oldcode < 0)
		return 0;
	finchar = oldcode;
	if (runs)
		put_unpacked(o, (uint8_t)finchar);
	else
		put(o, (uint8_t)finchar);
	for (;;)
	{
		NEXT_CODE(code);
		if (code < 0)
			break;
		if (code == 256)
		{
			for (i = 0; i < 256; i++)
				prefix[i] = 0;
			clear = 1;
			free_ent = 256;
			NEXT_CODE(code);
			if (code < 0)
				break;
		}
		incode = code;
		sp = 0;
		if (code >= free_ent)          /* the code being defined: the last string and its first byte */
		{
			if (code > free_ent)
				return -1;
			stack[sp++] = (uint8_t)finchar;
			code = oldcode;
		}
		while (code >= 256)
		{
			if (sp >= 65535)
				return -1;
			stack[sp++] = suffix[code];
			code = prefix[code];
		}
		stack[sp++] = (uint8_t)(finchar = suffix[code]);
		while (sp > 0)
			if (runs)
				put_unpacked(o, stack[--sp]);
			else
				put(o, stack[--sp]);
		if (free_ent < maxmaxcode)
		{
			prefix[free_ent] = (uint16_t)oldcode;
			suffix[free_ent] = (uint8_t)finchar;
			free_ent++;
		}
		oldcode = incode;
	}
#undef NEXT_CODE
	return 0;
}

/* A file's data by its method (ARC's numbers): 0 if it came out its length with the right CRC */
static int unarc(int method, const uint8_t *in, size_t clen, int maxbits, uint32_t len, uint16_t crc, uint8_t **data,
		 char *error, size_t esize)
{
	out_t o;
	size_t i;
	int bad = 0;

	if (!crc_table[1])
		for (i = 0; i < 256; i++)
		{
			uint16_t c = (uint16_t)i;
			int k;

			for (k = 0; k < 8; k++)
				c = (uint16_t)(c & 1 ? c >> 1 ^ 0xA001 : c >> 1);
			crc_table[i] = c;
		}
	memset(&o, 0, sizeof o);
	o.len = len;
	o.buf = malloc(len ? len : 1);
	switch (method)
	{
	case 1:
	case 2:
		for (i = 0; i < clen; i++)
			put(&o, in[i]);
		break;
	case 3:
		for (i = 0; i < clen; i++)
			put_unpacked(&o, in[i]);
		break;
	case 8:
		bad = lzw(in, clen, maxbits, 1, &o);
		break;
	case 9:
		bad = lzw(in, clen, 13, 0, &o);
		break;
	case 0x7F:
		bad = lzw(in, clen, maxbits, 0, &o);
		break;
	default:
		snprintf(error, esize, "The archive uses a way of packing files (method %d) that PiArcBoot doesn't read.", method);
		free(o.buf);
		return -1;
	}
	if (bad || o.at != len || o.crc != crc)
	{
		snprintf(error, esize, "The archive is damaged: a file in it doesn't unpack as its check says it should.");
		free(o.buf);
		return -1;
	}
	*data = o.buf;
	return 0;
}

/* ---- The list of files ---- */

static void add(adfs_file_t **files, int *n, const char *dir, const char *name, uint32_t load, uint32_t exec, uint32_t attr,
		uint8_t *data, uint32_t len, int is_dir)
{
	char path[600];
	adfs_file_t *f;

	snprintf(path, sizeof path, "%s%s%s", dir, dir[0] ? "." : "", name);
	*files = realloc(*files, (size_t)(*n + 1) * sizeof **files);
	f = &(*files)[(*n)++];
	memset(f, 0, sizeof *f);
	f->path = strdup(path);
	f->load = load;
	f->exec = exec;
	f->attr = attr;
	f->data = data;
	f->len = len;
	f->dir = is_dir;
}

/* A name from an archive as RISC OS has it: up to the first control character, with the . of a
   name from MS-DOS as RISC OS's / */
static void take_name(char *out, const uint8_t *p, size_t max)
{
	size_t i;

	for (i = 0; i < max && p[i] >= ' ' && p[i] != 0x7F; i++)
		out[i] = p[i] == '.' ? '/' : (char)p[i];
	out[i] = 0;
	if (!i)
		snprintf(out, 8, "File");
}

static int spark(const uint8_t *d, size_t len, adfs_file_t **files, int *n, char *error, size_t esize)
{
	char dir[600] = "";
	size_t depth[MAX_DEPTH], at = 0;
	int level = 0;

	while (at + 2 <= len && d[at] == 0x1A)
	{
		int method = d[at + 1] & 0x7F, riscos = d[at + 1] & 0x80;
		size_t need = 23 + (method > 1 ? 4 : 0) + (riscos ? 12 : 0), clen;
		const uint8_t *h = d + at + 2;
		char name[16];
		uint32_t load, exec, attr, olen;
		uint16_t crc;
		uint8_t *data;

		at += 2;
		if (!method)                    /* the end of a directory, or of the archive */
		{
			if (!level)
				return 0;
			dir[depth[--level]] = 0;
			continue;
		}
		if (at + need > len)
			break;
		take_name(name, h, 13);
		clen = rd32(h + 13);
		crc = (uint16_t)(h[21] | h[22] << 8);
		olen = method > 1 ? rd32(h + 23) : (uint32_t)clen;
		h += 23 + (method > 1 ? 4 : 0);
		load = riscos ? rd32(h) : 0xFFFFFD00u;          /* data, from MS-DOS */
		exec = riscos ? rd32(h + 4) : 0;
		attr = riscos ? rd32(h + 8) : 3;
		at += need;
		if (method == 2 && (load & 0xFFFFFF00u) == 0xFFFDDC00u)    /* a directory: its entries follow */
		{
			if (level == MAX_DEPTH)
				break;
			add(files, n, dir, name, load, exec, attr, NULL, 0, 1);
			depth[level++] = strlen(dir);
			snprintf(dir + strlen(dir), sizeof dir - strlen(dir), "%s%s", dir[0] ? "." : "", name);
			continue;
		}
		if (clen > len - at || olen > 64u << 20)
			break;
		if (unarc(method, d + at + (method == 8 || method == 0x7F), clen - (method == 8 || method == 0x7F),
			  method == 8 || method == 0x7F ? (clen ? d[at] : 0) : 13, olen, crc, &data, error, esize))
			return -1;
		add(files, n, dir, name, load, exec, attr & 0xFF, data, olen, 0);
		at += clen;
	}
	if (at >= len || d[at] != 0x1A)
		return 0;                       /* the end, with nothing after it, as some archives have */
	snprintf(error, esize, "The archive is damaged: an entry in it runs past its end.");
	return -1;
}

static int arcfs(const uint8_t *d, size_t len, adfs_file_t **files, int *n, char *error, size_t esize)
{
	char dir[600] = "";
	size_t depth[MAX_DEPTH], table, start, at;
	int level = 0;

	if (len < 96 || rd32(d + 24) != 0)
		goto damaged;
	table = rd32(d + 8);
	start = rd32(d + 12);
	if (table > len - 96 || start > len)
		goto damaged;
	for (at = 96; at + 36 <= 96 + table; at += 36)
	{
		const uint8_t *e = d + at;
		uint32_t olen = rd32(e + 12), load = rd32(e + 16), exec = rd32(e + 20), attr = rd32(e + 24), clen = rd32(e + 28), info = rd32(e + 32);
		char name[16];
		uint8_t *data;
		size_t where;
		int method;

		if (e[0] == 1)                  /* deleted */
			continue;
		if (e[0] == 0)                  /* the end of a directory */
		{
			if (level)
				dir[depth[--level]] = 0;
			continue;
		}
		take_name(name, e + 1, 11);
		if (info >> 31 && clen == 0xFFFFFFFFu && olen == 0xFFFFFFFFu)
		{
			if (level == MAX_DEPTH)
				goto damaged;
			add(files, n, dir, name, load, exec, attr & 0xFF, NULL, 0, 1);
			depth[level++] = strlen(dir);
			snprintf(dir + strlen(dir), sizeof dir - strlen(dir), "%s%s", dir[0] ? "." : "", name);
			continue;
		}
		where = start + (info & 0x7FFFFFFFu);
		if (where > len || clen > len - where || olen > 64u << 20)
			goto damaged;
		method = e[0] == 0x82 ? 2 : e[0] == 0x83 ? 3 : e[0] == 0x88 ? 8 : e[0] == 0xFF ? 0x7F : e[0];
		if (unarc(method, d + where, clen, (int)(attr >> 8 & 0xFF), olen, (uint16_t)(attr >> 16), &data, error, esize))
			return -1;
		add(files, n, dir, name, load, exec, attr & 0xFF, data, olen, 0);
	}
	return 0;
damaged:
	snprintf(error, esize, "The archive is damaged: its table of files doesn't fit it.");
	return -1;
}

int arc_is_archive(const uint8_t *d, size_t len)
{
	if (len >= 96 && !memcmp(d, "Archive", 8))
		return 1;
	/* &1A, a method, and a name of a printable character or more then a NUL within 13 bytes */
	if (len >= 29 && d[0] == 0x1A && (d[1] & 0x7F) >= 1 && (d[1] & 0x7F) <= 0x7F && d[2] >= ' ')
	{
		int i;

		for (i = 2; i < 15 && d[i] >= ' '; i++);
		return i < 15 && !d[i];
	}
	return 0;
}

int arc_files(const uint8_t *d, size_t len, adfs_file_t **out, char *error, size_t esize)
{
	adfs_file_t *files = NULL;
	int n = 0;

	if (!arc_is_archive(d, len))
	{
		snprintf(error, esize, "That isn't a Spark or ArcFS archive.");
		return -1;
	}
	if ((len >= 96 && !memcmp(d, "Archive", 8) ? arcfs : spark)(d, len, &files, &n, error, esize))
	{
		adfs_free(files, n);
		return -1;
	}
	*out = files;
	return n;
}

/* For piarcboot check: a Spark archive of a directory holding a stored file, a packed one and a
   crunched one, and an ArcFS archive of the same three. The crunched file's bytes come from ARC's
   own method. */
int arc_selftest(void)
{
	static const uint8_t text[] = "ABABABA AAAAAAAAAAAAAAAAAAAA ABABABA\n";
	uint8_t spk[512], afs[512], packed[64], crunched[64];
	size_t s = 0, a, tl = sizeof text - 1, pl = 0, cl;
	adfs_file_t *f;
	uint16_t crc = 0;
	char error[200];
	int n, i, k, ok = 1;

	/* packed: the run of 20 As is A &90 20 */
	for (i = 0; i < (int)tl; i++)
	{
		packed[pl++] = text[i];
		if (text[i] == 'A' && i + 1 < (int)tl && text[i + 1] == 'A')
		{
			for (k = i + 1; k < (int)tl && text[k] == 'A'; k++);
			packed[pl++] = 0x90;
			packed[pl++] = (uint8_t)(k - i);
			i = k - 1;
		}
	}
	/* crunched: the packed bytes as 9 bit LZW codes, each a byte of its own (no strings reused),
	   which any decoder must take as they are. The table only fills to 257 + codes - 1. */
	{
		uint32_t acc = 0;
		int bits = 0;

		cl = 0;
		crunched[cl++] = 12;
		for (i = 0; i < (int)pl; i++)
		{
			acc |= (uint32_t)packed[i] << bits;
			bits += 9;
			while (bits >= 8)
			{
				crunched[cl++] = (uint8_t)acc;
				acc >>= 8;
				bits -= 8;
			}
		}
		if (bits)
			crunched[cl++] = (uint8_t)acc;
	}
	for (i = 0; i < (int)tl; i++)
	{
		crc ^= text[i];
		for (k = 0; k < 8; k++)
			crc = (uint16_t)(crc & 1 ? crc >> 1 ^ 0xA001 : crc >> 1);
	}
	/* Spark */
	{
		static const struct { const char *name; int method; } e[3] = { { "Stored", 0x82 }, { "Packed", 0x83 }, { "Crunched", 0x88 } };
		uint8_t *h = spk;

		memset(spk, 0, sizeof spk);
		h[0] = 0x1A;
		h[1] = 0x82;
		memcpy(h + 2, "Dir", 3);
		h[30] = 0xDC;   /* load &FFFDDC00, at 29 */
		h[31] = 0xFD;
		h[32] = 0xFF;
		s = 41;
		for (i = 0; i < 3; i++)
		{
			const uint8_t *src = i == 0 ? text : i == 1 ? packed : crunched;
			size_t len = i == 0 ? tl : i == 1 ? pl : cl;

			h = spk + s;
			h[0] = 0x1A;
			h[1] = (uint8_t)e[i].method;
			memcpy(h + 2, e[i].name, strlen(e[i].name));
			h[15] = (uint8_t)len;
			h[23] = (uint8_t)crc;
			h[24] = (uint8_t)(crc >> 8);
			h[25] = (uint8_t)tl;
			h[30] = 0xFF;   /* load &FFFFFF00, text */
			h[31] = 0xFF;
			h[32] = 0xFF;
			h[37] = 3;      /* attributes, after the execution address at 33 */
			memcpy(h + 41, src, len);
			s += 41 + len;
		}
		spk[s++] = 0x1A;        /* the end of the directory, then of the archive */
		spk[s++] = 0x80;
		spk[s++] = 0x1A;
		spk[s++] = 0x80;
	}
	/* ArcFS: the header, the table and the data */
	{
		static const uint8_t kinds[3] = { 0x82, 0x83, 0x88 };
		static const char *const names[3] = { "Stored", "Packed", "Crunched" };
		size_t table = 36 * 5, data = 96 + table, at = 0;

		memset(afs, 0, sizeof afs);
		memcpy(afs, "Archive", 8);
		afs[8] = (uint8_t)table;
		afs[12] = (uint8_t)data;
		afs[13] = (uint8_t)(data >> 8);
		afs[16] = 40;
		memset(afs + 96 + 12, 0xFF, 4);         /* the directory: lengths of &FFFFFFFF, bit 31 of the info word */
		memset(afs + 96 + 28, 0xFF, 4);
		afs[96 + 35] = 0x80;
		afs[96] = 0x82;
		memcpy(afs + 97, "Dir", 3);
		for (i = 0; i < 3; i++)
		{
			uint8_t *e = afs + 96 + 36 * (i + 1);
			const uint8_t *src = i == 0 ? text : i == 1 ? packed : crunched + 1;
			size_t len = i == 0 ? tl : i == 1 ? pl : cl - 1;

			e[0] = kinds[i];
			memcpy(e + 1, names[i], strlen(names[i]));
			e[12] = (uint8_t)tl;
			memset(e + 16, 0xFF, 4);
			e[16] = 0x00;
			e[24] = 3;
			e[25] = 12;
			e[26] = (uint8_t)crc;
			e[27] = (uint8_t)(crc >> 8);
			e[28] = (uint8_t)len;
			e[32] = (uint8_t)at;
			memcpy(afs + data + at, src, len);
			at += len;
		}
		a = data + at;
	}
	for (k = 0; k < 2; k++)
	{
		n = arc_files(k ? afs : spk, k ? a : s, &f, error, sizeof error);
		ok &= n == 4 && f[0].dir && !strcmp(f[0].path, "Dir");
		for (i = 1; ok && i < 4; i++)
			ok &= !strncmp(f[i].path, "Dir.", 4) && f[i].len == tl && !memcmp(f[i].data, text, tl);
		if (n > 0)
			adfs_free(f, n);
	}
	return ok ? 0 : -1;
}
