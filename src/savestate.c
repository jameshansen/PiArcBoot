/* PiArcBoot: save states, the whole machine as it is at the end of a centisecond.

   The parts go one after another, each with a tag and its length, so that a state written by a
   build whose parts differ is turned away rather than misread. A state is taken into memory
   between steps, which is a copy of the RAM and some small structures, and squeezed and written to
   its file on a thread of its own, so the game doesn't stop for it. It is taken only while the
   floppy controller has no command going, when nothing points into a disc.

   A file is "PIARCBOOT-STATE\n", then 32 bit words, low byte first: the format's version, the
   CRC-32 of the ROM and the extension ROM, the size of the RAM, the floppy controller (0 the
   1772, 1 the 82C711), the length of the parts, and the width and height of the screenshot and
   its length squeezed. Then come the screenshot, a 32 bit &00RRGGBB word a pixel, and the parts,
   each squeezed by zlib. Reading one back that doesn't fit the machine as it is leaves the
   machine as it was.

   Free software under the GNU General Public License, version 2 or (at your option) any later
   version. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL.h>
#include <zlib.h>
#ifdef _WIN32
#include <windows.h>
#endif
#include "../arcem/armdefs.h"
#include "../arcem/arch/armarc.h"
#include "../arcem/arch/archio.h"
#include "../arcem/arch/keyboard.h"
#include "../arcem/snap.h"
#include "piarcboot.h"
#include "core.h"

#define MAGIC "PIARCBOOT-STATE\n"
#define VERSION 2
#define HEAD 48

void emu_snap(snap_t *s);                               /* emu.c */
void MEMC_Snap(ARMul_State *state, snap_t *s);          /* arch/armarc.c */
void screen_snap(ARMul_State *state, snap_t *s);        /* screen.c */
void I2C_Snap(snap_t *s);                               /* arch/i2c.c */
void ARM3_Snap(snap_t *s);                              /* arch/cp15.c */
void Sound_Snap(snap_t *s);                             /* arch/newsound.c */
void hostfs_snap(snap_t *s);                            /* hostfs.c */

void snap_bytes(snap_t *s, void *p, size_t n)
{
	if (s->saving)
	{
		if (s->len + n > s->cap)
		{
			size_t cap = (s->len + n) * 3 / 2 + 4096;
			uint8_t *b = realloc(s->buf, cap);

			if (!b)
			{
				s->bad = 1;
				return;
			}
			s->buf = b;
			s->cap = cap;
		}
		memcpy(s->buf + s->len, p, n);
		s->len += n;
	}
	else if (s->bad || s->at + n > s->len)
		s->bad = 1;
	else
	{
		memcpy(p, s->buf + s->at, n);
		s->at += n;
	}
}

static void memory_part(snap_t *s)
{
	MEMC_Snap(&statestr, s);
}

static void ioc_part(snap_t *s)
{
	SNAP(s, ioc);
}

static void screen_part(snap_t *s)
{
	screen_snap(&statestr, s);
}

/* The keyboard as the machine has been told it, all but the host's call for its lights */
static void keyboard_part(snap_t *s)
{
	arch_keyboard *k = statestr.Kbd;
	void (*leds)(uint8_t) = k->leds_changed;

	SNAP(s, *k);
	k->leds_changed = leds;
}

static void pc_floppy_part(snap_t *s)
{
	if (c711_on)
		c711_snap(s);
}

static const struct
{
	char tag[5];
	void (*fn)(snap_t *);
} parts[] = {
	{ "CPU ", emu_snap },           /* first: the memory's map follows the processor's mode */
	{ "MEMC", memory_part }, { "IOC ", ioc_part }, { "VIDC", screen_part }, { "KBD ", keyboard_part },
	{ "I2C ", I2C_Snap }, { "ARM3", ARM3_Snap }, { "SND ", Sound_Snap }, { "FDC ", fdc_snap },
	{ "711 ", pc_floppy_part }, { "HFS ", hostfs_snap },
};

/* Every part, each with its tag and its length */
static void all_parts(snap_t *s)
{
	size_t i;

	for (i = 0; i < sizeof parts / sizeof *parts && !s->bad; i++)
	{
		char tag[4];
		uint32_t len = 0;
		size_t at;

		memcpy(tag, parts[i].tag, 4);
		SNAP(s, tag);
		if (!s->saving && memcmp(tag, parts[i].tag, 4))
			s->bad = 1;
		at = s->saving ? s->len : s->at + 4;
		SNAP(s, len);
		parts[i].fn(s);
		if (s->saving && !s->bad)
		{
			len = (uint32_t)(s->len - at - 4);
			memcpy(s->buf + at, &len, 4);
		}
		else if (!s->saving && s->at != at + len)
			s->bad = 1;     /* a part of another length: another build's */
	}
}

static void wr32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static uint32_t rd32(const uint8_t *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

/* What a state has to have been taken on: the ROMs, the RAM and the floppy controller */
static void machine_head(uint8_t *h)
{
	static uint32_t rom_crc;

	if (!rom_crc)
	{
		uLong c = crc32(0, (const Bytef *)MEMC.ROMHigh, MEMC.ROMHighSize);

		if (MEMC.ROMLow)
			c = crc32(c, (const Bytef *)MEMC.ROMLow, MEMC.ROMLowSize);
		rom_crc = (uint32_t)c | 1;
	}
	memcpy(h, MAGIC, 16);
	wr32(h + 16, VERSION);
	wr32(h + 20, rom_crc);
	wr32(h + 24, MEMC.RAMSize);
	wr32(h + 28, (uint32_t)c711_on);
}

int state_ready(void)
{
	return fdc_idle();
}

/* ---- Writing ---- */

typedef struct
{
	char path[1024];
	uint8_t *raw;
	size_t len;
	uint32_t *shot;         /* the screenshot, or NULL */
	int sw, sh;
} job_t;

static SDL_atomic_t writing;

/* Squeezes the parts and writes the file under another name, then gives it its own, so that the
   state there before stays whole until the new one is */
static int write_state(job_t *j)
{
	uLong shot_len = j->shot ? (uLong)j->sw * j->sh * 4 : 0;
	uLongf slen = compressBound(shot_len), zlen = compressBound((uLong)j->len);
	uint8_t *out = malloc(HEAD + slen + zlen);
	char tmp[1100];
	int r = -1;

	if (!shot_len)
		slen = 0;
	if (out && (!shot_len || compress2(out + HEAD, &slen, (const Bytef *)j->shot, shot_len, 6) == Z_OK) &&
	    compress2(out + HEAD + slen, &zlen, j->raw, (uLong)j->len, 1) == Z_OK)
	{
		machine_head(out);
		wr32(out + 32, (uint32_t)j->len);
		wr32(out + 36, shot_len ? (uint32_t)j->sw : 0);
		wr32(out + 40, shot_len ? (uint32_t)j->sh : 0);
		wr32(out + 44, (uint32_t)slen);
		snprintf(tmp, sizeof tmp, "%s.new", j->path);
		if (!file_write(tmp, out, HEAD + slen + zlen))
		{
#ifdef _WIN32
			r = MoveFileExA(tmp, j->path, MOVEFILE_REPLACE_EXISTING) ? 0 : -1;
#else
			r = rename(tmp, j->path);
#endif
			if (r)
				remove(tmp);
		}
	}
	free(out);
	return r;
}

static int write_thread(void *p)
{
	job_t *j = p;

	write_state(j);
	free(j->raw);
	free(j->shot);
	free(j);
	SDL_AtomicSet(&writing, 0);
	return 0;
}

int state_writing(void)
{
	return SDL_AtomicGet(&writing);
}

/* Takes the machine as it is now and writes it to path, with a screenshot sw by sh if shot isn't
   NULL: on a thread of its own, or with wait set before it returns. -1 if it can't be taken just
   now, with a command going on the floppy controller or the last state still being written, or
   (with wait) can't be written. */
int state_save(const char *path, int wait, const uint32_t *shot, int sw, int sh)
{
	snap_t s = { 0 };
	job_t *j;
	SDL_Thread *t;
	int r;

	if (!fdc_idle() || !SDL_AtomicCAS(&writing, 0, 1))
		return -1;
	machine_head((uint8_t[HEAD]){ 0 });     /* the ROM's CRC, worked out now rather than on the thread */
	s.saving = 1;
	all_parts(&s);
	j = calloc(1, sizeof *j);
	if (s.bad || !j)
	{
		free(s.buf);
		free(j);
		SDL_AtomicSet(&writing, 0);
		return -1;
	}
	snprintf(j->path, sizeof j->path, "%s", path);
	j->raw = s.buf;
	j->len = s.len;
	if (shot && sw > 0 && sh > 0 && (j->shot = malloc((size_t)sw * sh * 4)))
	{
		memcpy(j->shot, shot, (size_t)sw * sh * 4);
		j->sw = sw;
		j->sh = sh;
	}
	if (!wait && (t = SDL_CreateThread(write_thread, "state", j)))
	{
		SDL_DetachThread(t);
		return 0;
	}
	r = write_state(j);
	free(j->raw);
	free(j->shot);
	free(j);
	SDL_AtomicSet(&writing, 0);
	return r;
}

/* ---- Reading back ---- */

/* The machine as it is now, *len bytes held in memory to go back to with state_put, or NULL if it
   can't be taken just now */
uint8_t *state_take(size_t *len)
{
	snap_t s = { 0 };

	if (!fdc_idle())
		return NULL;
	s.saving = 1;
	all_parts(&s);
	if (s.bad)
	{
		free(s.buf);
		return NULL;
	}
	*len = s.len;
	return s.buf;
}

/* Puts the machine back as a state's parts have it, with no command going on the floppy
   controller. 0, or -1 with the machine as it was. */
int state_put(const uint8_t *parts, size_t len)
{
	snap_t in = { 0 }, was = { 0 };
	int r;

	if (!fdc_idle())
		return -1;
	in.buf = (uint8_t *)parts;      /* only read */
	in.len = in.cap = len;
	was.saving = 1;                 /* the machine now, to go back to if the parts don't all fit */
	all_parts(&was);
	all_parts(&in);
	r = in.bad || in.at != in.len ? -1 : 0;
	if (r)
	{
		was.saving = was.bad = 0;
		was.at = 0;
		all_parts(&was);
	}
	free(was.buf);
	return r;
}

/* Puts the machine as the file has it, with no command going on the floppy controller. Returns
   0, or -1 with the reason in why and the machine as it was. */
int state_load(const char *path, char *why, size_t size)
{
	size_t flen;
	uint8_t *f = file_read(path, &flen), head[HEAD];
	uLongf len;
	uint8_t *parts;
	int r;

	snprintf(why, size, "The save couldn't be read.");
	if (!fdc_idle())
	{
		snprintf(why, size, "The floppy drive is busy just now.");
		free(f);
		return -1;
	}
	if (!f)
		return -1;
	machine_head(head);
	if (flen < HEAD || memcmp(f, MAGIC, 16))
	{
		free(f);
		return -1;
	}
	if (memcmp(f, head, 32))
	{
		snprintf(why, size, "This save was made with another version of PiArcBoot or its ROM, and can't be restored.");
		free(f);
		return -1;
	}
	len = rd32(f + 32);
	parts = malloc(len ? len : 1);
	if (!parts || rd32(f + 44) > flen - HEAD ||
	    uncompress(parts, &len, f + HEAD + rd32(f + 44), (uLong)(flen - HEAD - rd32(f + 44))) != Z_OK || len != rd32(f + 32))
	{
		free(parts);
		free(f);
		return -1;
	}
	free(f);
	if ((r = state_put(parts, len)))
		snprintf(why, size, "This save couldn't be restored.");
	free(parts);
	return r;
}

/* The screenshot kept in a state's file, *w by *h, or NULL if it has none or can't be read */
uint32_t *state_shot(const char *path, int *w, int *h)
{
	FILE *f = fopen(path, "rb");
	uint8_t head[HEAD], *z = NULL;
	uint32_t *px = NULL;
	uLongf len;

	*w = *h = 0;
	if (!f)
		return NULL;
	if (fread(head, 1, HEAD, f) == HEAD && !memcmp(head, MAGIC, 16) && rd32(head + 16) == VERSION &&
	    rd32(head + 36) && rd32(head + 36) <= 2048 && rd32(head + 40) && rd32(head + 40) <= 2048 &&
	    rd32(head + 44) <= 64 << 20 && (z = malloc(rd32(head + 44) + 1)) && fread(z, 1, rd32(head + 44), f) == rd32(head + 44))
	{
		len = (uLongf)rd32(head + 36) * rd32(head + 40) * 4;
		px = malloc(len);
		if (px && uncompress((Bytef *)px, &len, z, rd32(head + 44)) == Z_OK && len == (uLongf)rd32(head + 36) * rd32(head + 40) * 4)
		{
			*w = (int)rd32(head + 36);
			*h = (int)rd32(head + 40);
		}
		else
		{
			free(px);
			px = NULL;
		}
	}
	free(z);
	fclose(f);
	return px;
}
