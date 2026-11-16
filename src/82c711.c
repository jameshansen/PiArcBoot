/* PiArcBoot: the A5000's IOEB and its 82C711, for the games whose discs are 1.6MB. The floppy
   disc controller is written from the 765 data sheet, the rest from what RISC OS 3.11 asks of
   the parts.

   The IOEB answers at &03350050 with 5 in its low nibble, which is how RISC OS knows it is there,
   and its latch at &03350048 picks the VIDC's clock. The 82C711 sits in the IO space beside the
   IOC's, with a PC's port n at &03010000 + 4n: its configuration registers at &3F0 and &3F1, a
   serial port whose scratch register tells RISC OS it is an 82C711, and a 765 behind the PC/AT
   registers at &3F2 to &3F7 (DOR, main status, data, disc changed and data rate).

   The 765 moves data by DMA. Its data request goes to the IOC's fast interrupt input FH0, and
   the FIQ handler takes each byte at &03012000, or the last at &0302A000, which gives the
   terminal count. Its interrupt goes to IRQ B bit 4, the selected drive's index pulse to IRQ A
   bit 2, and the IOC control register's bit 2 reads high with a high density disc in. It works
   the drives and discs wd1772.c has, sector by sector as they pass the head, as the 1772 does:
   a search gives up after two index pulses, and a byte not taken by the time the next arrives
   is an overrun.

   As with the 1772, unless machine_real_drive is set the heads step in a tenth of a millisecond,
   and a read on an ordinary track finds its sector at once and gives each byte 4 us after the
   last was taken. Other tracks, Read ID, writes and formats keep a real drive's timing.

   Free software under the GNU General Public License, version 2 or (at your option) any later
   version. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../arcem/armdefs.h"
#include "../arcem/arch/armarc.h"
#include "../arcem/arch/archio.h"
#include "../arcem/arch/displaydev.h"
#include "piarcboot.h"
#include "disc.h"
#include "core.h"
#include "../arcem/snap.h"

#define REV ((uint64_t)MACHINE_HZ / 5)          /* a revolution at 300 rpm */
#define US ((uint64_t)MACHINE_HZ / 1000000)
#define NEVER UINT64_MAX
#define FAST_STEP ((uint64_t)MACHINE_HZ / 10000)        /* a step, when the drive needn't keep a real one's pace */
#define FAST_GAP (4 * US)                               /* from a byte being taken to the next, in a fast read */

#define PORTS 0x03010000u                       /* port 0 */
#define DACK 0x2000u                            /* from PORTS: a DMA byte */
#define DACK_TC 0x1a000u                        /* and the last, with the terminal count */
#define IRQ_765 (1u << 12)                      /* IRQ B bit 4 */
#define IRQ_INDEX (1u << 2)                     /* IRQ A bit 2 */
#define FIQ_DRQ 1u                              /* FH0 */
#define DENSITY_IN 0x04                         /* in the IOC control register */

#define DOR_RESET 0x04                          /* low holds the 765 in reset */
#define DOR_LINES 0x08                          /* the interrupt and data request lines are driven */

#define MSR_RQM 0x80
#define MSR_DIO 0x40
#define MSR_CB 0x10

#define ST0_ABNORMAL 0x40
#define ST0_INVALID 0x80
#define ST0_READY 0xc0                          /* a drive's ready line changed, as after a reset */
#define ST0_SE 0x20
#define ST1_EN 0x80
#define ST1_DE 0x20
#define ST1_OR 0x10
#define ST1_ND 0x04
#define ST1_NW 0x02
#define ST1_MA 0x01
#define ST2_DD 0x20
#define ST2_WC 0x10
#define ST2_BC 0x02

/* What the 765 is executing */
enum { X_NONE, X_SEARCH, X_READ_ID, X_READ, X_READ_END, X_WRITE, X_FORMAT_WAIT, X_FORMAT, X_NEXT, X_END };

int c711_on;
static int fast;                        /* the read under way is on an ordinary track: its bytes come as soon as they are taken */

static struct
{
	uint8_t dor, ccr, srt;
	uint8_t cmd[9];
	int ncmd, need;                 /* command bytes in, and how many the command has */
	uint8_t res[7];
	int nres, ires;                 /* result bytes, and how many have been read */
	int exec;
	uint64_t when, deadline;
	int irq, drq, tc;
	uint8_t data;                   /* the byte the data request is for */
	struct { uint8_t st0, pcn; } pending[8];        /* interrupts for Sense Interrupt Status */
	int npending;
	int seek_to[4], seek_head[4];   /* where each drive's head is going, or -1 */
	uint64_t seek_when[4];
	uint8_t st0, st1, st2, c, h, r, n;
	dside_t *side;
	int si, k, len, id_seen;
	uint64_t base;
	uint8_t buf[128 << 7];          /* a sector being written */
	uint8_t ids[4 * 256];           /* the IDs of a track being formatted */
	uint64_t index_seen;
	uint8_t cr[16], cfg_index, last_3f0, uart[8];
	int cfg;
} K;

static int tracing = -1;

static int trace(void)
{
	if (tracing < 0)
		tracing = getenv("PIARCBOOT_FDC") != NULL;
	return tracing;
}

static drive_t *sel(void)
{
	return &floppy[K.dor & 3];
}

static int spinning(void)
{
	return K.dor >> (4 + (K.dor & 3)) & 1 && sel()->disc;
}

static int head(void)
{
	return K.cmd[1] >> 2 & 1;
}

static dside_t *cur_side(void)
{
	drive_t *d = sel();

	return d->disc && d->cyl >= 0 && d->cyl < DISC_CYLS ? &d->disc->side[d->cyl][head()] : NULL;
}

/* The sectors the data rate and the command's MFM bit can read */
static int density(void)
{
	int mfm = K.cmd[0] & 0x40;

	switch (K.ccr & 3)
	{
		case 0:
		return mfm ? DENS_HIGH : 0;     /* 500 kbit/s */
		case 2:
		return mfm ? DENS_DOUBLE : DENS_SINGLE; /* 250 kbit/s */
		default:
		return 0;                       /* 300 kbit/s and 1 Mbit/s: nothing on these discs */
	}
}

static uint64_t byte_time(const dside_t *side)
{
	return REV / (side && side->bytes ? side->bytes : DISC_DD_BYTES);
}

static uint64_t next_index(uint64_t t)
{
	return (t / REV + 1) * REV;
}

/* The next sector of the density whose ID address mark reaches the head after t, or -1 */
static int next_id(dside_t *side, int dens, uint64_t t, uint64_t *start)
{
	uint64_t rev = t - t % REV, bc = byte_time(side);
	int pass, i;

	for (pass = 0; pass < 2; pass++)
		for (i = 0; i < side->n; i++)
		{
			uint64_t at = rev + pass * REV + (uint64_t)side->s[i].pos * bc;

			if (side->s[i].density == dens && at > t)
			{
				*start = at;
				return i;
			}
		}
	return -1;
}

static void lines(void)
{
	ARMul_State *state = &statestr;
	int on = K.dor & DOR_LINES;

	if (on && (K.irq || K.npending))
		ioc.IRQStatus |= IRQ_765;
	else
		ioc.IRQStatus &= ~IRQ_765;
	if (on && K.drq)
		ioc.FIRQStatus |= FIQ_DRQ;
	else
		ioc.FIRQStatus &= ~FIQ_DRQ;
	IO_UpdateNirq(state);
	IO_UpdateNfiq(state);
}

static void pend(uint8_t st0, uint8_t pcn)
{
	if (K.npending < 8)
	{
		K.pending[K.npending].st0 = st0;
		K.pending[K.npending].pcn = pcn;
		K.npending++;
	}
	lines();
}

static void result(int n, int irq)
{
	if (trace())
		fprintf(stderr, "765 %9.4f   %02x done: %02x %02x %02x  %d %d %d %d\n", (double)machine_clock() / MACHINE_HZ, K.cmd[0], K.res[0],
			n > 1 ? K.res[1] : 0, n > 2 ? K.res[2] : 0, K.res[3], K.res[4], K.res[5], K.res[6]);
	K.nres = n;
	K.ires = 0;
	K.exec = X_NONE;
	K.irq = irq;
	K.drq = 0;
	fast = 0;
	lines();
}

static void end_at(uint64_t t)
{
	K.exec = X_END;
	K.when = t;
}

static void search(uint64_t t)
{
	K.exec = (K.cmd[0] & 0x1f) == 0x0a ? X_READ_ID : X_SEARCH;
	K.id_seen = 0;
	fast = 0;
	K.deadline = spinning() ? next_index(t) + REV : NEVER;  /* two index pulses */
	K.when = t;
}

/* A sector done: the next, or the end at the terminal count or the last sector of the track */
static void next_sector(uint64_t t)
{
	int last = K.r == K.cmd[6];

	if (!K.tc && last)
		K.st0 |= ST0_ABNORMAL, K.st1 |= ST1_EN;
	if (K.tc || last)
	{
		if (last)
			K.r = 1, K.c++;
		else
			K.r++;
		end_at(t);
	}
	else
	{
		K.r++;
		search(t);
	}
}

static void overrun(uint64_t t)
{
	K.st0 |= ST0_ABNORMAL;
	K.st1 |= ST1_OR;
	K.drq = 0;
	lines();
	end_at(t);
}

static void act(void)
{
	uint64_t t = K.when, start, bc = byte_time(K.side);
	dsector_t *s;
	drive_t *d = sel();
	int i;

	switch (K.exec)
	{
		case X_SEARCH:
		case X_READ_ID:
		{
			dside_t *side = cur_side();
			int dens = density();

			if (!machine_real_drive && K.exec == X_SEARCH && ((K.cmd[0] & 0x1f) == 0x06 || (K.cmd[0] & 0x1f) == 0x0c) && spinning() &&
			    side && dens == side->s[0].density && disc_ordinary(side, d->cyl, head()))
			{
				/* an ordinary track: the sector wanted is under the head now */
				for (i = 0; i < side->n && (side->s[i].c != K.c || side->s[i].h != K.h || side->s[i].r != K.r || side->s[i].n != K.n); i++)
					;
				if (i < side->n)
				{
					K.id_seen = 1;
					K.side = side;
					K.si = i;
					K.base = t;
					K.k = 0;
					K.len = 128 << K.n;
					fast = 1;
					K.exec = X_READ;
					K.when = t + FAST_GAP;
					return;
				}
			}
			for (;;)
			{
				i = side && dens ? next_id(side, dens, t, &start) : -1;
				if (i < 0 || start > K.deadline)
				{
					if (K.deadline == NEVER)
					{
						K.when = NEVER; /* no index pulses: it waits for a reset */
						return;
					}
					K.st0 |= ST0_ABNORMAL;
					K.st1 |= K.id_seen ? ST1_ND : ST1_MA;
					end_at(K.deadline);
					return;
				}
				s = &side->s[i];
				K.id_seen = 1;
				t = start;
				bc = byte_time(side);
				if (K.exec == X_READ_ID)
				{
					if (s->id_crc)
						continue;
					K.c = s->c;
					K.h = s->h;
					K.r = s->r;
					K.n = s->n;
					end_at(start + 7 * bc);
					return;
				}
				if (s->r == K.r && s->h == K.h && s->n == K.n && s->c != K.c)
					K.st2 |= ST2_WC | (s->c == 0xff ? ST2_BC : 0);
				if (s->c != K.c || s->h != K.h || s->r != K.r || s->n != K.n)
					continue;
				if (s->id_crc)
				{
					K.st1 |= ST1_DE;
					continue;
				}
				K.st1 &= ~ST1_DE;
				K.st2 &= ~(ST2_WC | ST2_BC);
				K.side = side;
				K.si = i;
				K.base = start;
				K.k = 0;
				K.len = 128 << (K.n > 7 ? 7 : K.n);
				if ((K.cmd[0] & 0x1f) == 0x06 || (K.cmd[0] & 0x1f) == 0x0c)
				{
					K.exec = X_READ;
					K.when = start + 46 * bc;
				}
				else
				{
					memset(K.buf, 0, K.len);
					K.exec = X_WRITE;
					K.when = start + 34 * bc;
				}
				return;
			}
		}

		case X_READ:
		s = &K.side->s[K.si];
		if (K.drq)
		{
			overrun(t);
			break;
		}
		if (K.tc)                       /* the transfer is over: the rest of the sector goes by */
		{
			K.exec = X_READ_END;
			K.when = fast ? t + bc : K.base + (46 + K.len + 2) * bc;
			break;
		}
		K.data = K.k < (int)s->len ? s->data[K.k] : 0;
		K.drq = 1;
		lines();
		if (++K.k < K.len)
			K.when = fast ? t + bc : K.base + (46 + K.k) * bc;     /* fast: sooner, once this one is taken */
		else
		{
			K.exec = X_READ_END;
			K.when = fast ? t + bc : K.base + (46 + K.len + 2) * bc;
		}
		break;

		case X_READ_END:
		s = &K.side->s[K.si];
		if (K.drq)
			overrun(t);
		else if (s->data_crc || s->len != (uint32_t)K.len)
		{
			K.st0 |= ST0_ABNORMAL;
			K.st1 |= ST1_DE;
			K.st2 |= ST2_DD;
			end_at(t);
		}
		else
			next_sector(t);
		break;

		case X_WRITE:                   /* asks for byte k, once the one before is in */
		if (K.drq)
		{
			overrun(t);
			break;
		}
		if (K.k < K.len && !K.tc)
		{
			K.drq = 1;
			lines();
			K.k++;
			K.when = K.base + (34 + K.k) * bc;
			break;
		}
		s = &K.side->s[K.si];
		if (s->len != (uint32_t)K.len)
		{
			s->data = realloc(s->data, K.len);
			s->len = K.len;
		}
		memcpy(s->data, K.buf, K.len);
		s->data_crc = 0;
		K.side->changed = 1;
		d->disc->changed = 1;
		K.exec = X_NEXT;
		K.when = K.base + (34 + K.len + 2) * bc;
		break;

		case X_NEXT:
		next_sector(t);
		break;

		case X_FORMAT_WAIT:             /* the track starts at the index pulse */
		K.base = t;
		K.k = 0;
		K.exec = X_FORMAT;
		break;

		case X_FORMAT:                  /* four bytes of ID for each sector, as it is written */
		{
			int n = K.cmd[3], size = 128 << (K.cmd[2] > 7 ? 7 : K.cmd[2]), dens = density();
			int bytes = dens == DENS_HIGH ? DISC_HD_BYTES : dens == DENS_SINGLE ? DISC_DD_BYTES / 2 : DISC_DD_BYTES;
			int span = 62 + size + K.cmd[4];

			bc = REV / bytes;
			if (K.drq)
			{
				overrun(t);
				break;
			}
			if (K.k < 4 * n && !K.tc)
			{
				K.drq = 1;
				lines();
				K.k++;
				K.when = K.base + (80 + (uint64_t)(K.k / 4) * span + K.k % 4) * bc;
				break;
			}
			s = calloc(n ? n : 1, sizeof *s);
			for (i = 0; i < n; i++)
			{
				s[i].c = K.ids[4 * i];
				s[i].h = K.ids[4 * i + 1];
				s[i].r = K.ids[4 * i + 2];
				s[i].n = K.ids[4 * i + 3];
				s[i].density = (uint8_t)dens;
				s[i].pos = -1;
				s[i].len = size;
				s[i].data = malloc(size);
				memset(s[i].data, K.cmd[5], size);
			}
			if (d->disc && d->cyl < DISC_CYLS)
			{
				disc_set_side(d->disc, d->cyl, head(), s, n, bytes);
				d->disc->changed = 1;
			}
			else
			{
				for (i = 0; i < n; i++)
					free(s[i].data);
				free(s);
			}
			end_at(next_index(t));
			break;
		}

		case X_END:
		K.res[0] = K.st0;
		K.res[1] = K.st1;
		K.res[2] = K.st2;
		K.res[3] = K.c;
		K.res[4] = K.h;
		K.res[5] = K.r;
		K.res[6] = K.n;
		result(7, 1);
		break;
	}
}

/* A step of a Seek or Recalibrate */
static void step(int i)
{
	static const unsigned rate_us[4] = { 1000, 1667, 2000, 500 }; /* an SRT unit at each data rate */
	drive_t *d = &floppy[i];

	if (d->cyl != K.seek_to[i])
	{
		d->cyl += d->cyl < K.seek_to[i] ? 1 : -1;
		if (d->disc)
			d->changed = 0;
	}
	if (d->cyl == K.seek_to[i])
	{
		K.seek_to[i] = -1;
		pend((uint8_t)(ST0_SE | K.seek_head[i] << 2 | i), (uint8_t)d->cyl);
		return;
	}
	K.seek_when[i] += machine_real_drive ? (16 - K.srt) * rate_us[K.ccr & 3] * US : FAST_STEP;
}

static void catch_up(void)
{
	ARMul_State *state = &statestr;
	uint64_t now = machine_clock();
	drive_t *d = sel();
	int guard = 0, i;

	while (K.exec && K.when <= now && guard++ < 100000)
		act();
	for (i = 0; i < 4; i++)
		while (K.seek_to[i] >= 0 && K.seek_when[i] <= now)
			step(i);
	if (!spinning())
		K.index_seen = now / REV;
	else if (now / REV != K.index_seen)
	{
		K.index_seen = now / REV;
		ioc.IRQStatus |= IRQ_INDEX;
		IO_UpdateNirq(state);
	}
	if (d->disc && d->disc->side[0][0].bytes == DISC_HD_BYTES)
		ioc.ControlRegInputData |= DENSITY_IN;
	else
		ioc.ControlRegInputData &= ~DENSITY_IN;
}

/* ---- The 765's registers ---- */

static int cmd_len(uint8_t c)
{
	switch (c & 0x1f)
	{
		case 0x03: case 0x0f:
		return 3;
		case 0x04: case 0x07: case 0x0a:
		return 2;
		case 0x05: case 0x06: case 0x09: case 0x0c:
		return 9;
		case 0x0d:
		return 6;
		default:
		return 1;               /* Sense Interrupt Status, and the commands it hasn't */
	}
}

static void run(uint64_t t)
{
	uint8_t c = K.cmd[0] & 0x1f;
	int dr = K.cmd[1] & 3;
	drive_t *d = &floppy[dr];

	if (trace())
		fprintf(stderr, "765 %9.4f %02x %02x %02x %02x %02x %02x %02x %02x %02x, DOR %02x CCR %d, head at %d\n", (double)t / MACHINE_HZ,
			K.cmd[0], K.cmd[1], K.cmd[2], K.cmd[3], K.cmd[4], K.cmd[5], K.cmd[6], K.cmd[7], K.cmd[8], K.dor, K.ccr, sel()->cyl);
	switch (c)
	{
		case 0x03:                      /* Specify: the step rate */
		K.srt = K.cmd[1] >> 4;
		return;

		case 0x04:                      /* Sense Drive Status */
		K.res[0] = (uint8_t)(0x28 | (!d->disc || d->wp ? 0x40 : 0) | (d->cyl == 0 ? 0x10 : 0) | head() << 2 | dr);
		result(1, 0);
		return;

		case 0x07:                      /* Recalibrate */
		case 0x0f:                      /* Seek */
		K.seek_to[dr] = c == 0x07 ? 0 : K.cmd[2] < DISC_CYLS ? K.cmd[2] : DISC_CYLS - 1;
		K.seek_head[dr] = head();
		K.seek_when[dr] = t;
		return;

		case 0x08:                      /* Sense Interrupt Status */
		if (!K.npending)
		{
			K.res[0] = ST0_INVALID;
			result(1, 0);
			return;
		}
		K.res[0] = K.pending[0].st0;
		K.res[1] = K.pending[0].pcn;
		memmove(K.pending, K.pending + 1, --K.npending * sizeof *K.pending);
		result(2, 0);
		return;

		case 0x05: case 0x06: case 0x09: case 0x0a: case 0x0c: case 0x0d:
		break;

		default:
		K.res[0] = ST0_INVALID;
		result(1, 0);
		return;
	}
	K.st0 = (uint8_t)(head() << 2 | dr);
	K.st1 = K.st2 = 0;
	K.tc = 0;
	K.c = K.cmd[2];
	K.h = K.cmd[3];
	K.r = K.cmd[4];
	K.n = K.cmd[5];
	K.side = NULL;
	if ((c == 0x05 || c == 0x09 || c == 0x0d) && (!sel()->disc || sel()->wp))
	{
		K.st0 |= ST0_ABNORMAL;
		K.st1 |= ST1_NW;
		end_at(t);
		return;
	}
	if (c == 0x06 || c == 0x0c)
		machine_disc_reads++;
	if (c == 0x0d)
	{
		K.exec = X_FORMAT_WAIT;
		K.when = spinning() ? next_index(t) : NEVER;
		return;
	}
	search(t);
}

static uint8_t msr(void)
{
	uint8_t v = 0;
	int i;

	if (!(K.dor & DOR_RESET))
		return 0;
	for (i = 0; i < 4; i++)
		if (K.seek_to[i] >= 0)
			v |= 1 << i;
	if (K.nres)
		return v | MSR_RQM | MSR_DIO | MSR_CB;
	if (K.exec)
		return v | MSR_CB;
	return v | MSR_RQM | (K.ncmd ? MSR_CB : 0);
}

static uint8_t data_read(void)
{
	uint8_t v;

	if (!K.nres)
		return 0xff;
	v = K.res[K.ires++];
	if (K.ires == K.nres)
		K.nres = 0;
	if (K.irq)                              /* reading the result clears the interrupt */
	{
		K.irq = 0;
		lines();
	}
	return v;
}

static void data_write(uint8_t v)
{
	if (K.nres || K.exec || !(K.dor & DOR_RESET))
		return;
	if (!K.ncmd)
		K.need = cmd_len(v);
	K.cmd[K.ncmd++] = v;
	if (K.ncmd == K.need)
	{
		memset(K.cmd + K.ncmd, 0, sizeof K.cmd - K.ncmd);
		K.ncmd = 0;
		run(machine_clock());
	}
}

static void dor_write(uint8_t v)
{
	int i;

	for (i = 0; i < 4; i++)                 /* a motor going off: what was written goes back to the image */
		if (K.dor >> (4 + i) & 1 && !(v >> (4 + i) & 1) && floppy[i].disc && floppy[i].disc->changed && !floppy[i].wp)
			disc_save(floppy[i].disc);
	if (!(v & DOR_RESET))
	{
		K.ncmd = K.nres = K.npending = 0;
		K.exec = X_NONE;
		K.irq = K.drq = fast = 0;
		for (i = 0; i < 4; i++)
			K.seek_to[i] = -1;
	}
	else if (!(K.dor & DOR_RESET))          /* out of reset: each drive's ready line has changed */
		for (i = 0; i < 4; i++)
			pend((uint8_t)(ST0_READY | i), (uint8_t)floppy[i].cyl);
	K.dor = v;
	lines();
}

/* ---- DMA ---- */

static uint8_t dma_read(int tc)
{
	if (K.drq)
	{
		K.drq = 0;
		lines();
		if (fast && K.when > machine_clock() + FAST_GAP)       /* the next byte comes soon after */
			K.when = machine_clock() + FAST_GAP;
	}
	if (tc && K.exec)
		K.tc = 1;
	return K.data;
}

static void dma_write(uint8_t v, int tc)
{
	if (K.drq)
	{
		K.drq = 0;
		if (K.exec == X_WRITE)
			K.buf[K.k - 1] = v;
		else if (K.exec == X_FORMAT && K.k <= (int)sizeof K.ids)
			K.ids[K.k - 1] = v;
		lines();
	}
	if (tc && K.exec)
		K.tc = 1;
}

/* ---- The 82C711's other ports ---- */

/* The serial port, from configuration register 2, or 0 when it is off. Only its scratch register
   holds what is written; the rest read as a port with nothing to do. */
static unsigned uart(void)
{
	static const unsigned at[4] = { 0x3f8, 0x2f8, 0x338, 0x238 };

	return K.cr[2] & 4 ? at[K.cr[2] & 3] : 0;
}

static uint8_t port_read(unsigned p)
{
	unsigned u = uart();

	if (K.cfg && p == 0x3f1)
		return K.cr[K.cfg_index & 15];
	if (u && p >= u && p < u + 8)
		return p - u == 2 ? 0x01 : p - u == 5 ? 0x60 : p - u == 7 ? K.uart[7] : 0;
	switch (p)
	{
		case 0x3f4:
		return msr();
		case 0x3f5:
		return data_read();
		case 0x3f7:
		return !sel()->disc || sel()->changed ? 0x80 : 0;
	}
	return 0xff;
}

static void port_write(unsigned p, uint8_t v)
{
	unsigned u = uart();

	if (p == 0x3f0)                         /* &55 twice to configure, &AA to stop */
	{
		if (K.cfg)
		{
			if (v == 0xaa)
				K.cfg = 0;
			else
				K.cfg_index = v;
		}
		else if (v == 0x55 && K.last_3f0 == 0x55)
		{
			K.cfg = 1;
			v = 0;
		}
		K.last_3f0 = v;
		return;
	}
	if (K.cfg && p == 0x3f1)
	{
		K.cr[K.cfg_index & 15] = v;
		return;
	}
	if (u && p >= u && p < u + 8)
	{
		K.uart[p - u] = v;
		return;
	}
	switch (p)
	{
		case 0x3f2:
		dor_write(v);
		break;
		case 0x3f5:
		data_write(v);
		break;
		case 0x3f7:
		K.ccr = v & 3;
		break;
	}
}

static bool io_access(ARMul_State *state, ARMword a, ARMword *data, bool write)
{
	uint32_t off;
	uint8_t v = (uint8_t)*data;

	if (a & (1u << 21))                     /* the IOC's space */
	{
		unsigned bank = a >> 16 & 7, speed = a >> 19 & 3;

		if (bank == 1)                  /* where the 1772 would be */
		{
			if (!write)
				*data = 0xff;
			return true;
		}
		if (bank != 5 || speed != 2)
			return false;
		switch (a & 0xfffc)
		{
			case 0x50:              /* the IOEB is here */
			if (!write)
				*data = 5;
			return true;
			case 0x70:              /* the monitor lead's ID lines, with nothing on them */
			if (!write)
				*data = 0x0f;
			return true;
			case 0x48:              /* the VIDC's clock and sync polarity */
			if (write)
			{
				ioc.IOEBControlReg = v & 0x0f;
				DisplayDev_Current->IOEBCRWrite(state, v);
			}
			return true;
			case 0x18:              /* the printer latches: the drives are the 82C711's here */
			case 0x40:
			return write;
		}
		return false;
	}
	off = a - PORTS;
	if (a < PORTS || off >= DACK_TC + 0x2000 || (off >= 0x1000 && off < DACK))
		return false;
	catch_up();
	if (off < 0x1000)
	{
		if (write)
			port_write(off >> 2, v);
		else
			*data = port_read(off >> 2);
	}
	else if (write)
		dma_write(v, off >= DACK_TC);
	else
		*data = dma_read(off >= DACK_TC);
	return true;
}

/* ---- The rest of the machine ---- */

void c711_start(int on)
{
	int i;

	c711_on = on;
	IO_Hook = on ? io_access : NULL;
	memset(&K, 0, sizeof K);
	for (i = 0; i < 4; i++)
		K.seek_to[i] = -1;
	K.ccr = 2;
	K.cr[2] = 0x1c;                         /* the serial port at &3F8 */
}

void c711_regular(void)
{
	catch_up();
}

int c711_fast(void)
{
	return fast;
}

int c711_motor(void)
{
	return (K.dor & 0xf0) != 0;
}

/* For a save state: no command is executing, so nothing points into a disc */
int c711_idle(void)
{
	return K.exec == X_NONE;
}

/* The 82C711's registers, taken with no command executing */
void c711_snap(snap_t *s)
{
	SNAP(s, K);
	if (!s->saving)
	{
		K.side = NULL;
		fast = 0;
	}
}
