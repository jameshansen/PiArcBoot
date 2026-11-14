/* PiArcBoot: ADFS D format floppy images made from files, for games that come as an application
   rather than as a disc image. D format is the 800K format with the old free space map and the
   77 entry directories, which RISC OS 3.11 reads with the 1772. Laid out from FileCore's own
   definitions in the RISC OS Open sources:

     &000  the free space map: start (sector 0) and length (sector 1) of each free area, 3 bytes
           each in 256 byte units, with the disc's size, name, id and boot option, and a check
           byte at the end of each sector
     &400  the root directory, &800 bytes: "Nick", 26 byte entries (name, load and execution
           addresses, length, where it is on the disc in 256 byte units, attributes), then the
           tail with the parent, title and name, and a check byte over the whole

   Everything else follows the root, each object in one piece starting at a 1024 byte sector, and
   the rest of the disc is one free area. */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "piarcboot.h"

#define DISC_SIZE (800 * 1024)       /* 80 tracks of two sides of five 1024 byte sectors */
#define TRACK 10240
#define MAX_SIZE (DISC_SIZE + 4 * TRACK) /* FileCore takes up to 4 tracks more */
#define SECTOR 1024
#define ROOT 0x400
#define DIR_SIZE 0x800
#define ENTRY 26
#define MAX_ENTRIES 77
#define TAIL (DIR_SIZE - 41)            /* the last entry marker, then the tail */

typedef struct node
{
	char name[11];
	int dir;
	uint32_t load, exec, attr, len, at;
	const uint8_t *data;
	struct node *child, *next;
} node_t;

static void put24(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
}

static void put32(uint8_t *p, uint32_t v)
{
	put24(p, v);
	p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

/* A name in an entry, ended by a CR when it is shorter than 10 */
static void put_name(uint8_t *p, const char *name)
{
	size_t n = strlen(name);

	memcpy(p, name, n);
	if (n < 10)
		p[n] = 13;
}

/* The free space map's check byte: its first 255 bytes added from the last to the first, each
   carry going into the next sum */
static uint8_t map_check(const uint8_t *p)
{
	unsigned sum = 0, carry = 0;
	int i;

	for (i = 254; i >= 0; i--)
	{
		sum += p[i] + carry;
		carry = sum >> 8;
		sum &= 255;
	}
	return (uint8_t)sum;
}

/* A directory's check byte, over its entries and its tail, a word at a time where they are words */
static uint8_t dir_check(const uint8_t *d, int entries)
{
	uint32_t r = 0;
	int end = 5 + entries * ENTRY, p;

#define MIX(v) (r = (v) ^ (r >> 13 | r << 19))
	for (p = 0; p < (end & ~3); p += 4)
		MIX(get32(d + p));
	for (; p < end; p++)
		MIX(d[p]);
	for (p = TAIL + 1; p < DIR_SIZE - 4; p += 4)
		MIX(get32(d + p));
#undef MIX
	r ^= r >> 16;
	r ^= r >> 8;
	return (uint8_t)r;
}

static int name_cmp(const char *a, const char *b)
{
	for (; *a && toupper((unsigned char)*a) == toupper((unsigned char)*b); a++, b++);
	return toupper((unsigned char)*a) - toupper((unsigned char)*b);
}

/* The child of dir called name, made if it isn't there */
static node_t *child(node_t *dir, const char *name, int is_dir)
{
	node_t **p, *n;

	for (p = &dir->child; *p && name_cmp((*p)->name, name) < 0; p = &(*p)->next);
	if (*p && !name_cmp((*p)->name, name))
		return *p;
	n = calloc(1, sizeof *n);
	snprintf(n->name, sizeof n->name, "%s", name);
	n->dir = is_dir;
	n->next = *p;
	*p = n;
	return n;
}

static void free_tree(node_t *n)
{
	while (n)
	{
		node_t *next = n->next;

		free_tree(n->child);
		free(n);
		n = next;
	}
}

/* Gives each object its place on the disc, after the ones before. Returns 0, or -1 when the
   disc is full or a directory has more than it can hold. */
static int place(node_t *dir, uint32_t *next)
{
	node_t *n;
	int count = 0;

	for (n = dir->child; n; n = n->next)
	{
		if (++count > MAX_ENTRIES)
			return -1;
		/* an object starts at a sector, 1024 bytes on this disc: FileCore reads it from there */
		n->at = *next;
		*next += n->dir ? DIR_SIZE : n->len ? (n->len + SECTOR - 1) / SECTOR * SECTOR : SECTOR;
		if (*next > MAX_SIZE)
			return -1;
	}
	for (n = dir->child; n; n = n->next)
		if (n->dir && place(n, next))
			return -1;
	return 0;
}

static void write_dir(uint8_t *img, const node_t *dir, uint32_t parent)
{
	uint8_t *d = img + dir->at;
	const node_t *n;
	int count = 0;

	memcpy(d + 1, "Nick", 4);
	for (n = dir->child; n; n = n->next, count++)
	{
		uint8_t *e = d + 5 + count * ENTRY;

		put_name(e, n->name);
		put32(e + 10, n->load);
		put32(e + 14, n->exec);
		put32(e + 18, n->dir ? DIR_SIZE : n->len);
		put24(e + 22, n->at / 256);
		/* RISC OS's attributes have R, W, L, public R and public W in bits 0, 1, 3, 4 and 5; an
		   entry keeps L in bit 2, the public ones in bits 5 and 6, and bit 3 for a directory */
		e[25] = (uint8_t)((n->dir ? 8 : (n->attr & 3) | (n->attr & 0x30) << 1) | (n->attr & 8 ? 4 : 0));
		if (n->dir)
			write_dir(img, n, dir->at);
		else if (n->len)
			memcpy(img + n->at, n->data, n->len);
	}
	put24(d + TAIL + 3, parent / 256);
	put_name(d + TAIL + 6, dir->name);              /* the title, 19 bytes */
	put_name(d + TAIL + 25, dir->name);
	memcpy(d + DIR_SIZE - 5, "Nick", 4);
	d[DIR_SIZE - 1] = dir_check(d, count);
}

uint8_t *adfs_make(const adfs_file_t *files, int n, const char *disc_name, size_t *len, char *error, size_t error_size)
{
	node_t root = { "$", 1, 0, 0, 0, 0, ROOT, NULL, NULL, NULL };
	uint32_t next = ROOT + DIR_SIZE, bytes, size;
	uint8_t *img;
	int i;

	for (i = 0; i < n; i++)
	{
		char part[256];
		const char *p = files[i].path;
		node_t *at = &root;

		while (*p)
		{
			size_t k = strcspn(p, ".");

			snprintf(part, sizeof part, "%.*s", (int)(k < 10 ? k : 10), p);
			p += k + (p[k] == '.');
			if (!part[0])
				continue;
			at = child(at, part, *p != 0 || files[i].dir);
		}
		if (at != &root && !at->dir)
		{
			at->load = files[i].load;
			at->exec = files[i].exec;
			at->attr = files[i].attr;
			at->data = files[i].data;
			at->len = files[i].len;
		}
		else if (at != &root)
			at->attr = files[i].attr;
	}
	if (place(&root, &next))
	{
		snprintf(error, error_size, "The game's files don't fit on one floppy disc, or a folder of it holds more than 77 files.");
		free_tree(root.child);
		return NULL;
	}

	/* 80 tracks, or as many more as the files need */
	bytes = next <= DISC_SIZE ? DISC_SIZE : (next + TRACK - 1) / TRACK * TRACK;
	size = bytes / 256;
	img = calloc(1, bytes);
	write_dir(img, &root, ROOT);
	/* one free area, from the end of the files to the end of the disc */
	if (next < bytes)
	{
		put24(img, next / 256);
		put24(img + 256, size - next / 256);
		img[256 + 0xFE] = 3;
	}
	put24(img + 0xFC, size);
	for (i = 0; i < 10; i++)        /* the disc's name, its letters taken alternately from the two sectors */
		img[(i & 1 ? 256 + 0xF6 : 0xF7) + i / 2] = (uint8_t)(i < (int)strlen(disc_name) ? disc_name[i] : ' ');
	img[256 + 0xFB] = 0x41;         /* the disc's id */
	img[256 + 0xFC] = 0x52;
	img[0xFF] = map_check(img);
	img[256 + 0xFF] = map_check(img + 256);
	free_tree(root.child);
	*len = bytes;
	return img;
}

/* The files of a zip as RISC OS has them, under prefix (a folder in the zip, or ""): their names
   with the zip's / turned back into RISC OS's . and its . into /, and their load and execution
   addresses from the zip, or from a ,xxx file type at the end of the name, or as data. Returns
   how many, with the contents in *data to be freed with adfs_free. */
int adfs_from_zip(const uint8_t *zip, size_t zlen, const char *prefix, adfs_file_t **out)
{
	zipent_t *ents;
	int n = zip_list(zip, zlen, &ents), i, count = 0;
	size_t plen = strlen(prefix);

	*out = calloc(n ? n : 1, sizeof **out);
	for (i = 0; i < n; i++)
	{
		adfs_file_t *f = &(*out)[count];
		const char *name = ents[i].name;
		char path[256], *c;
		size_t size, k;
		int dir = name[0] && name[strlen(name) - 1] == '/';
		unsigned type = 0xFFD;

		if (strncmp(name, prefix, plen) || !name[plen])
			continue;
		snprintf(path, sizeof path, "%s", name + plen);
		if (dir)
			path[strlen(path) - 1] = 0;
		for (c = path; *c; c++)         /* and a space, which RISC OS names can't hold, as a hard space */
			*c = *c == '/' ? '.' : *c == '.' ? '/' : *c == ' ' ? (char)0xA0 : *c;
		k = strlen(path);
		if (k > 4 && path[k - 4] == ',' && strspn(path + k - 3, "0123456789abcdefABCDEF") == 3)
		{
			type = (unsigned)strtoul(path + k - 3, NULL, 16);
			path[k - 4] = 0;
		}
		f->path = strdup(path);
		f->dir = dir;
		f->attr = ents[i].riscos ? ents[i].attr : 3;
		if (ents[i].riscos)
		{
			f->load = ents[i].load;
			f->exec = ents[i].exec;
		}
		else
		{
			f->load = 0xFFF00000u | type << 8 | 0x41;       /* stamped 1 January 1992 */
			f->exec = 0x42F2D200u;
		}
		if (!dir)
		{
			uint8_t *d = zip_get(zip, zlen, &ents[i], &size);

			if (!d)
			{
				free((char *)f->path);
				continue;
			}
			f->data = d;
			f->len = (uint32_t)size;
		}
		count++;
	}
	free(ents);
	return count;
}

/* An archive's files with the folder its applications are in made the top: Apps.Games.!Foo comes
   out as !Foo, and what isn't in that folder, a package's RiscPkg say, is left out. Applications
   in more than one folder keep the folder they share. Returns how many files are left. */
int adfs_app_root(adfs_file_t *f, int n)
{
	char root[512] = "";
	int have = 0, i, k = 0;
	size_t rl;

	for (i = 0; i < n; i++)
	{
		const char *p = f[i].path, *app = p[0] == '!' ? p : strstr(p, ".!");
		size_t len, c = 0;

		if (!app)
			continue;
		len = (size_t)(app - p);        /* the folder holding the application */
		if (!have)
		{
			snprintf(root, sizeof root, "%.*s", (int)len, p);
			have = 1;
			continue;
		}
		while (root[c] && c < len && root[c] == p[c])
			c++;
		if (!((!root[c] || root[c] == '.') && (c == len || p[c] == '.')))
			while (c > 0 && root[c] != '.')         /* back to the last whole folder in common */
				c--;
		root[c] = 0;
	}
	if (!root[0])
		return n;
	rl = strlen(root);
	for (i = 0; i < n; i++)
	{
		if (!strncmp(f[i].path, root, rl) && f[i].path[rl] == '.')
		{
			char *inner = strdup(f[i].path + rl + 1);

			free((char *)f[i].path);
			f[k] = f[i];
			f[k++].path = inner;
		}
		else
		{
			free((char *)f[i].path);
			free((uint8_t *)f[i].data);
		}
	}
	return k;
}

void adfs_free(adfs_file_t *files, int n)
{
	while (n-- > 0)
	{
		free((char *)files[n].path);
		free((uint8_t *)files[n].data);
	}
	free(files);
}

/* For piarcboot check: a disc made from two files, one in a folder, read back */
int adfs_selftest(void)
{
	static const uint8_t text[] = "PRINT \"Hello\"";
	adfs_file_t f[2] = {
		{ "!Test.!Run", 0xFFFFEB41, 0x42F2D200, 3, text, sizeof text - 1, 0 },
		{ "ReadMe", 0xFFFFFF41, 0x42F2D200, 3, text, 5, 0 },
	};
	char error[200];
	size_t len;
	uint8_t *img = adfs_make(f, 2, "Test", &len, error, sizeof error), *root, *sub;
	int fail = 0;

	if (!img || len != DISC_SIZE)
		return 1;
	root = img + ROOT;
	fail |= map_check(img) != img[0xFF] || map_check(img + 256) != img[0x1FF];
	fail |= memcmp(root + 1, "Nick", 4) || dir_check(root, 2) != root[DIR_SIZE - 1];
	fail |= memcmp(root + 5, "!Test\r", 6) || root[5 + 25] != 8;              /* sorted, ! before R */
	fail |= memcmp(root + 5 + ENTRY, "ReadMe\r", 7) || get32(root + 5 + ENTRY + 18) != 5;
	sub = img + (root[5 + 22] | root[5 + 23] << 8) * 256;
	fail |= memcmp(sub + 5, "!Run\r", 5) || (sub[DIR_SIZE - 41 + 3] | sub[DIR_SIZE - 41 + 4] << 8) != ROOT / 256;
	fail |= memcmp(img + (sub[5 + 22] | sub[5 + 23] << 8) * 256, text, sizeof text - 1);
	fail |= (img[0] | img[1] << 8) != (ROOT + 2 * DIR_SIZE + 2 * SECTOR) / 256;  /* free after root, !Test, !Run and ReadMe */
	fail |= (root[5 + ENTRY + 22] | root[5 + ENTRY + 23] << 8) * 256 % SECTOR != 0;
	fail |= img[0xF7] != 'T' || img[0x1F6] != 'e' || img[0xF8] != 's';
	free(img);
	return fail;
}
