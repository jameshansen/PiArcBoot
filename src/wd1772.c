/* PiArcBoot: the WD1772 floppy disc controller and its four drives, written from the WD1772
   data sheet, for ArcEm's IOC (arch/archio.c calls the FDC_ functions of arch/fdc1772.h).

   A disc turns at 300 rpm, so a revolution is 200 ms and an index pulse starts each one. Its
   track sides are lists of sectors with their places on the track (disc.c), and the 1772 works
   through them as they pass the head: an ID field is only seen once it has gone by, a search
   gives up after its revolutions, a sector's data comes a byte every 32 us, and a byte the
   program hasn't taken by the time the next arrives is lost. That is what copy protection
   checks: sectors that read with CRC errors, IDs in an order of their own, sizes that differ,
   the time from one sector to the next, and tracks past 79.

   Unless machine_real_drive is set, the drive is quicker than that where it can be without a
   program telling: it spins up, steps and settles in a tenth of a millisecond, and on an ordinary
   track, the kind a format program lays down, the sector a read asks for is under the head at once
   and each of its bytes comes 4 us after the last one was taken, or a real byte's time after it
   arrived if it wasn't. The program still gets a gap between bytes for its other interrupts. Any
   other track, Read Address, Read Track and every write keep a real drive's timing, as copy
   protection works through those.

   The 1772's interrupt and data request lines go to the IOC's fast interrupt inputs FH1 and
   FH0. Printer latch A selects the drive (bits 0 to 3, low for selected) and the side (bit 4,
   high for side 0); latch B selects the density (bit 1, high for single) and holds the 1772 in
   reset (bit 3, low).

   Free software under the GNU General Public License, version 2 or (at your option) any later
   version. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../arcem/armdefs.h"
#include "../arcem/arch/armarc.h"
#include "../arcem/arch/archio.h"
#include "../arcem/arch/fdc1772.h"
#include "piarcboot.h"
#include "disc.h"
#include "core.h"
#include "../arcem/snap.h"

#define REV ((uint64_t)MACHINE_HZ / 5)          /* a revolution at 300 rpm */
#define MS ((uint64_t)MACHINE_HZ / 1000)
#define INDEX_PULSE (2 * MS)                    /* how long the index line stays active */
#define FAST_STEP (MS / 10)                     /* a step, a settle or the spin-up, when the drive needn't keep a real one's pace */
#define FAST_GAP ((uint64_t)MACHINE_HZ / 250000)  /* 4 us from a byte being taken to the next, in a fast read */
#define NEVER UINT64_MAX

/* Status bits */
#define ST_BUSY 0x01
#define ST_INDEX 0x02           /* Type I */
#define ST_DRQ 0x02             /* Types II and III */
#define ST_TR00 0x04            /* Type I */
#define ST_LOST 0x04            /* Types II and III */
#define ST_CRC 0x08
#define ST_RNF 0x10             /* record not found, or seek error */
#define ST_SPUN 0x20            /* Type I: spin-up done */
#define ST_WP 0x40
#define ST_MOTOR 0x80

enum
{
	P_IDLE, P_SPINUP, P_SETTLE, P_STEP, P_VERIFY, P_SEARCH, P_READ, P_READ_CRC,
	P_WR_FIRST, P_WR_CHECK, P_WR_REQ, P_WR_TAKE, P_WR_END, P_RA, P_RA_DATA,
	P_RT_WAIT, P_RT, P_WT_WAIT, P_WT, P_END
};

static struct
{
	uint8_t status, track, sector, data, cmd;
	int type1;              /* the status register has the Type I meaning */
	int busy, intrq, drq, force_latched, index_irq;
	int motor, spun;
	uint64_t motor_off;
	int phase;
	uint64_t when;          /* when the phase's next action is due */
	int dir;                /* the last step's direction, +1 in towards the centre */
	int restore, single;    /* a Restore, a single step */
	uint8_t latch_a, latch_b;
	int cur;                /* the selected drive, or -1 */
	int sd, reset;
	uint64_t deadline;
	int crc_seen, si, k, len;
	dside_t *side;          /* the side the command is working on */
	uint64_t base;          /* when the sector it found starts to pass the head */
	uint8_t *buf;           /* Read Track's bytes, Write Track's and Write Sector's */
	int buflen;
	uint64_t last_index;
} F;

drive_t floppy[4];

int machine_disc_reads;
int machine_real_drive;
static int fast;                        /* the read under way is on an ordinary track: its bytes come as soon as they are taken */
static int tracing = -1;                /* PIARCBOOT_FDC: each command and how it ended, on stderr */

static uint64_t now_cycles(void)
{
	return machine_clock();
}

static int trace(void)
{
	if (tracing < 0)
		tracing = getenv("PIARCBOOT_FDC") != NULL;
	return tracing;
}

static drive_t *sel(void)
{
	return F.cur >= 0 ? &floppy[F.cur] : NULL;
}

static int side_sel(void)
{
	return F.latch_a & 0x10 ? 0 : 1;
}

/* The disc turns, with index pulses, while the motor is on and a disc is in the selected drive */
static int spinning(void)
{
	drive_t *d = sel();

	return F.motor && d && d->disc;
}

static dside_t *cur_side(void)
{
	drive_t *d = sel();

	if (!d || !d->disc || d->cyl < 0 || d->cyl >= DISC_CYLS)
		return NULL;
	return &d->disc->side[d->cyl][side_sel()];
}

static uint64_t next_index(uint64_t t)
{
	return (t / REV + 1) * REV;
}

static int readable(const dsector_t *s)
{
	return F.sd ? s->density == DENS_SINGLE : s->density == DENS_DOUBLE;
}

static uint64_t byte_time(const dside_t *side)
{
	return REV / (side && side->bytes ? side->bytes : DISC_DD_BYTES);
}

/* The next sector the 1772 can read whose ID address mark reaches the head after t. Returns its
   index on the side and when its address mark arrives, or -1. */
static int next_id(dside_t *side, uint64_t t, uint64_t *start)
{
	uint64_t rev = t - t % REV, bc = byte_time(side);
	int pass, i;

	if (!side)
		return -1;
	for (pass = 0; pass < 2; pass++)
		for (i = 0; i < side->n; i++)
		{
			uint64_t at = rev + pass * REV + (uint64_t)side->s[i].pos * bc;

			if (readable(&side->s[i]) && at > t)
			{
				*start = at;
				return i;
			}
		}
	return -1;
}

/* How long a step, the head settling or the spin-up takes */
static uint64_t step_time(uint64_t real)
{
	return machine_real_drive ? real : FAST_STEP;
}

static void lines(void)
{
	ARMul_State *state = &statestr;

	if (F.intrq)
		ioc.FIRQStatus |= FIQ_FDIRQ;
	else
		ioc.FIRQStatus &= ~FIQ_FDIRQ;
	if (F.drq)
		ioc.FIRQStatus |= FIQ_FDDRQ;
	else
		ioc.FIRQStatus &= ~FIQ_FDDRQ;
	IO_UpdateNfiq(state);
}

static void set_drq(uint8_t v)
{
	if (F.drq)
		F.status |= ST_LOST;
	F.data = v;
	F.drq = 1;
	lines();
}

/* Writes back the discs that have been written to */
static void save_discs(void)
{
	int i;

	for (i = 0; i < 4; i++)
		if (floppy[i].disc && floppy[i].disc->changed && !floppy[i].wp)
			disc_save(floppy[i].disc);
}

/* The command is over at time t: an interrupt, and the motor goes off 9 index pulses later */
static void finish(uint64_t t)
{
	if (trace())
		fprintf(stderr, "fdc %9.4f   %02x done: status %02x track %d sector %d\n", (double)t / MACHINE_HZ, F.cmd, F.status, F.track, F.sector);
	F.busy = 0;
	F.phase = P_IDLE;
	F.intrq = 1;
	F.motor_off = spinning() ? next_index(t) + 8 * REV : NEVER;
	fast = 0;
	lines();
}

static int step_rate_ms(void)
{
	static const int ms[4] = { 6, 12, 2, 3 };

	return ms[F.cmd & 3];
}

static void start_search(uint64_t t, int revs)
{
	F.phase = F.cmd < 0x80 ? P_VERIFY : F.cmd >= 0xc0 ? P_RA : P_SEARCH;
	F.crc_seen = 0;
	fast = 0;
	F.deadline = spinning() ? next_index(t) + (uint64_t)(revs - 1) * REV : NEVER;
	F.when = t;
}

/* After the spin-up, or straight away: the command proper */
static void begin(uint64_t t)
{
	uint8_t c = F.cmd;

	if (c < 0x80)                           /* Type I */
	{
		F.restore = (c & 0xf0) == 0x00;
		F.single = c >= 0x20;
		if ((c & 0xe0) == 0x40)
			F.dir = 1;
		else if ((c & 0xe0) == 0x60)
			F.dir = -1;
		F.k = 0;
		F.phase = P_STEP;
		F.when = t;
		return;
	}
	if ((c & 0xe0) == 0xa0 || (c & 0xf0) == 0xf0)  /* a write, to a protected disc */
	{
		drive_t *d = sel();

		if (d && d->wp)
		{
			F.status |= ST_WP;
			finish(t);
			return;
		}
	}
	F.phase = P_SETTLE;
	F.when = c & 0x04 ? t + step_time(15 * MS) : t;        /* E: 15 ms for the head to settle (30 on the 1770) */
}

static void command(uint8_t c, uint64_t t)
{
	if (trace())
	{
		drive_t *d = sel();

		fprintf(stderr, "fdc %9.4f %02x%s track %d sector %d data %d, drive %d head at %d side %d%s%s\n", (double)t / MACHINE_HZ, c,
			F.busy && (c & 0xf0) != 0xd0 ? " (busy, ignored)" : "", F.track, F.sector, F.data, F.cur, d ? d->cyl : -1,
			side_sel(), F.sd ? " single density" : "", F.reset ? " in reset" : "");
	}
	if ((c & 0xf0) == 0xd0)                 /* Force Interrupt */
	{
		if (F.busy)
		{
			F.busy = 0;
			F.phase = P_IDLE;
			F.motor_off = spinning() ? next_index(t) + 8 * REV : NEVER;
			fast = 0;
		}
		else
			F.type1 = 1;
		F.drq = 0;
		F.index_irq = (c & 0x04) != 0;
		F.last_index = t / REV;
		F.force_latched = (c & 0x08) != 0;
		F.intrq = F.force_latched;
		lines();
		return;
	}
	if (F.busy || F.reset)
		return;
	F.cmd = c;
	F.busy = 1;
	F.intrq = 0;
	F.drq = 0;
	F.type1 = c < 0x80;
	F.status = 0;
	F.motor_off = NEVER;
	F.side = NULL;
	if ((c & 0xe0) == 0x80)
		machine_disc_reads++;
	lines();
	if (!F.motor)
	{
		F.motor = 1;
		if (!(c & 0x08))                /* h clear: spin up for 6 index pulses first */
		{
			F.spun = 0;
			F.phase = P_SPINUP;
			F.when = spinning() ? (machine_real_drive ? next_index(t) + 5 * REV : t + FAST_STEP) : NEVER;
			return;
		}
	}
	F.spun = 1;
	begin(t);
}

/* What the side being formatted holds, from the bytes Write Track was given: F5 writes an A1
   sync byte, F6 a C2, F7 the two CRC bytes */
static void parse_format(int bytes)
{
	dsector_t *s = NULL;
	int n = 0, i, at = 0, id = -1, id_pos = 0;
	drive_t *d = sel();

	for (i = 0; i < F.buflen; i++)
	{
		uint8_t b = F.buf[i];

		if (b == 0xf7)
		{
			at += 2;
			continue;
		}
		if (i >= 3 && F.buf[i - 1] == 0xf5 && F.buf[i - 2] == 0xf5 && F.buf[i - 3] == 0xf5)
		{
			if (b == 0xfe && i + 4 < F.buflen)
			{
				s = realloc(s, (n + 1) * sizeof *s);
				memset(&s[n], 0, sizeof *s);
				s[n].c = F.buf[i + 1];
				s[n].h = F.buf[i + 2];
				s[n].r = F.buf[i + 3];
				s[n].n = F.buf[i + 4];
				s[n].density = F.sd ? DENS_SINGLE : DENS_DOUBLE;
				s[n].pos = at;
				s[n].len = 128u << (s[n].n & 3);
				s[n].data = calloc(1, s[n].len);
				s[n].data_crc = 1;      /* no data field yet */
				id = n++;
				id_pos = i;
			}
			else if ((b == 0xfb || b == 0xf8) && id >= 0 && i - id_pos < 64)
			{
				int k;

				for (k = 0; k < (int)s[id].len && i + 1 + k < F.buflen; k++)
					s[id].data[k] = F.buf[i + 1 + k];
				s[id].data_crc = 0;
				id = -1;
			}
		}
		at++;
	}
	if (d && d->disc && d->cyl < DISC_CYLS)
		disc_set_side(d->disc, d->cyl, side_sel(), s, n, bytes);
	else
	{
		for (i = 0; i < n; i++)
			free(s[i].data);
		free(s);
	}
}

/* Carries out the action due at F.when */
static void act(void)
{
	uint64_t t = F.when, start, bc;
	drive_t *d = sel();
	dside_t *side;
	dsector_t *s;
	int i;

	switch (F.phase)
	{
		case P_SPINUP:
		F.spun = 1;
		begin(t);
		break;

		case P_STEP:                    /* one step at a time, step rate apart */
		{
			int target_reached = 0;

			if (F.restore || (F.cmd & 0xf0) == 0x10)
			{
				if (F.restore ? d && d->cyl == 0 : F.track == F.data)
					target_reached = 1;
				else
					F.dir = F.restore || F.data < F.track ? -1 : 1;
			}
			else if (F.single && F.k)
				target_reached = 1;     /* the single step is done */
			if (!target_reached && F.dir < 0 && d && d->cyl == 0)
			{
				F.track = 0;            /* stepping out with the head at track 0 */
				target_reached = 1;
			}
			if (!target_reached && F.k >= 255)     /* 255 steps and still not there: a seek error */
			{
				F.status |= ST_RNF;
				F.phase = P_END;
				F.when = t;
				break;
			}
			if (target_reached)
			{
				F.k = 0;
				if (F.restore)
					F.track = 0;
				if (F.cmd & 0x04)       /* V: 15 ms to settle, then an ID to verify the track */
					start_search(t + step_time(15 * MS), 5);
				else
				{
					F.phase = P_END;
					F.when = t;
				}
				break;
			}
			if (d)
			{
				d->cyl += F.dir;
				if (d->cyl < 0)
					d->cyl = 0;
				if (d->cyl >= DISC_CYLS)
					d->cyl = DISC_CYLS - 1;
			}
			if (!F.single || F.cmd & 0x10)
				F.track = (uint8_t)(F.track + F.dir);
			F.k++;
			F.when = t + step_time(step_rate_ms() * MS);
			break;
		}

		case P_SETTLE:
		F.k = 0;
		if ((F.cmd & 0xf0) == 0xe0 || (F.cmd & 0xf0) == 0xf0)  /* Read Track and Write Track wait for the index */
		{
			F.phase = (F.cmd & 0xf0) == 0xe0 ? P_RT_WAIT : P_WT_WAIT;
			if ((F.cmd & 0xf0) == 0xf0)
				set_drq(F.data);        /* Write Track asks for its first byte at once */
			F.when = spinning() ? next_index(t) : NEVER;
		}
		else
			start_search(t, (F.cmd & 0xf0) == 0xc0 ? 6 : 5);
		break;

		case P_VERIFY:                  /* the first good ID on the track has to carry the track register's number */
		case P_SEARCH:                  /* the ID with the track and sector registers' numbers */
		case P_RA:                      /* any ID */
		side = cur_side();
		if (!machine_real_drive && F.phase != P_RA && spinning() && disc_ordinary(side, d->cyl, side_sel()) && readable(&side->s[0]))
		{
			/* an ordinary track: the ID wanted is under the head now */
			for (i = 0; i < side->n && (side->s[i].c != F.track || (F.phase == P_SEARCH && side->s[i].r != F.sector)); i++)
				;
			if (i < side->n && F.phase == P_VERIFY)
			{
				F.phase = P_END;
				F.when = t + FAST_STEP;
				return;
			}
			if (i < side->n && (F.cmd & 0xe0) == 0x80)
			{
				F.side = side;
				F.si = i;
				F.base = t;
				F.k = 0;
				F.len = 128 << (side->s[i].n & 3);
				fast = 1;
				F.phase = P_READ;
				F.when = t + FAST_GAP;
				return;
			}
		}
		for (;;)
		{
			i = next_id(side, t, &start);
			if (i < 0 || start + 7 * byte_time(side) > F.deadline)
			{
				if (F.deadline == NEVER)
				{
					F.when = NEVER; /* no index pulses: it waits for a Force Interrupt */
					return;
				}
				F.status |= ST_RNF | (F.crc_seen ? ST_CRC : 0);
				F.phase = P_END;
				F.when = F.deadline;
				return;
			}
			s = &side->s[i];
			bc = byte_time(side);
			if (F.phase == P_RA)
			{
				F.side = side;
				F.si = i;
				F.base = start;
				F.k = 0;
				F.phase = P_RA_DATA;
				F.when = start + 2 * bc;
				return;
			}
			if (s->id_crc)
			{
				if (s->c == F.track && (F.phase == P_VERIFY || s->r == F.sector))
					F.crc_seen = 1;
				t = start;
				continue;
			}
			if (F.phase == P_VERIFY)
			{
				if (s->c == F.track)
				{
					F.phase = P_END;
					F.when = start + 7 * bc;
					return;
				}
				t = start;
				continue;
			}
			if (s->c != F.track || s->r != F.sector)
			{
				t = start;
				continue;
			}
			F.side = side;
			F.si = i;
			F.base = start;
			F.k = 0;
			F.len = 128 << (s->n & 3);
			if ((F.cmd & 0xe0) == 0x80)     /* Read Sector: the first byte is in 46 bytes' time */
			{
				F.phase = P_READ;
				F.when = start + 46 * bc;
			}
			else                            /* Write Sector: the first data request 2 bytes after the ID */
			{
				F.buf = realloc(F.buf, F.len);
				F.phase = P_WR_FIRST;
				F.when = start + 9 * bc;
			}
			return;
		}

		case P_READ:
		s = &F.side->s[F.si];
		set_drq(F.k < (int)s->len ? s->data[F.k] : 0);
		bc = byte_time(F.side);
		if (++F.k < F.len)
			F.when = fast ? t + bc : F.base + (46 + F.k) * bc;     /* fast: sooner, once this one is taken */
		else
		{
			F.phase = P_READ_CRC;
			F.when = fast ? t + bc : F.base + (46 + F.len + 1) * bc;
		}
		break;

		case P_READ_CRC:
		s = &F.side->s[F.si];
		if (s->data_crc || s->n > 3 || s->len != (uint32_t)F.len)
		{
			F.status |= ST_CRC;
			finish(t);
		}
		else if (F.cmd & 0x10)          /* m: the next sector */
		{
			F.sector++;
			start_search(t, 5);
		}
		else
			finish(t);
		break;

		case P_WR_FIRST:
		set_drq(F.data);
		F.phase = P_WR_CHECK;
		F.when = F.base + 18 * byte_time(F.side);
		break;

		case P_WR_CHECK:                /* the first byte has to be there 11 bytes after the ID */
		if (F.drq)
		{
			F.status |= ST_LOST;
			finish(t);
			break;
		}
		F.buf[0] = F.data;
		F.k = 1;
		F.phase = F.len > 1 ? P_WR_REQ : P_WR_END;
		F.when = F.len > 1 ? F.base + 45 * byte_time(F.side) : F.base + (45 + F.len + 2) * byte_time(F.side);
		break;

		case P_WR_REQ:
		F.drq = 1;
		lines();
		F.phase = P_WR_TAKE;
		F.when = t + byte_time(F.side);
		break;

		case P_WR_TAKE:
		if (F.drq)
		{
			F.status |= ST_LOST;
			F.drq = 0;
			lines();
			F.buf[F.k] = 0;
		}
		else
			F.buf[F.k] = F.data;
		bc = byte_time(F.side);
		if (++F.k < F.len)
		{
			F.phase = P_WR_REQ;
			F.when = F.base + (44 + F.k) * bc;
		}
		else
		{
			F.phase = P_WR_END;
			F.when = F.base + (45 + F.len + 2) * bc;
		}
		break;

		case P_WR_END:
		s = &F.side->s[F.si];
		if (s->len != (uint32_t)F.len)
		{
			s->data = realloc(s->data, F.len);
			s->len = F.len;
		}
		memcpy(s->data, F.buf, F.len);
		s->data_crc = 0;
		F.side->changed = 1;
		if (d && d->disc)
			d->disc->changed = 1;
		if (F.cmd & 0x10)
		{
			F.sector++;
			start_search(t, 5);
		}
		else
			finish(t);
		break;

		case P_RA_DATA:
		{
			uint16_t crc;
			uint8_t id[6];

			s = &F.side->s[F.si];
			crc = disc_id_crc(s);
			id[0] = s->c;
			id[1] = s->h;
			id[2] = s->r;
			id[3] = s->n;
			id[4] = (uint8_t)(crc >> 8);
			id[5] = (uint8_t)(s->id_crc ? crc ^ 0x5a : crc);
			set_drq(id[F.k]);
			if (++F.k < 6)
				F.when = F.base + (2 + F.k) * byte_time(F.side);
			else
			{
				F.sector = s->c;
				if (s->id_crc)
					F.status |= ST_CRC;
				F.phase = P_END;
				F.when = F.base + 8 * byte_time(F.side);
			}
			break;
		}

		case P_RT_WAIT:                 /* a revolution of bytes at the 1772's own rate */
		{
			dside_t readable_part;

			side = cur_side();
			F.side = NULL;
			F.buflen = F.sd ? DISC_DD_BYTES / 2 : DISC_DD_BYTES;
			F.buf = realloc(F.buf, F.buflen);
			memset(F.buf, 0x4e, F.buflen);
			if (side && side->bytes == F.buflen)
			{
				/* only the sectors it can read show as such; the rest is gap to it */
				readable_part = *side;
				readable_part.s = malloc((side->n ? side->n : 1) * sizeof(dsector_t));
				readable_part.n = 0;
				for (i = 0; i < side->n; i++)
					if (readable(&side->s[i]))
						readable_part.s[readable_part.n++] = side->s[i];
				disc_track_bytes(&readable_part, F.buf, NULL);
				free(readable_part.s);
			}
			F.base = t;
			F.k = 0;
			F.phase = P_RT;
			F.when = t + REV / F.buflen;
			break;
		}

		case P_RT:
		set_drq(F.buf[F.k]);
		if (++F.k < F.buflen)
			F.when = F.base + (F.k + 1) * (REV / F.buflen);
		else
		{
			F.phase = P_END;
			F.when = F.base + REV;
		}
		break;

		case P_WT_WAIT:                 /* the first byte has to be there by the index pulse */
		if (F.drq)
		{
			F.status |= ST_LOST;
			finish(t);
			break;
		}
		side = cur_side();
		F.side = side;
		F.len = side ? side->bytes : DISC_DD_BYTES;
		F.buf = realloc(F.buf, F.len);
		F.buf[0] = F.data;
		F.buflen = 1;
		F.base = t;
		F.drq = 1;
		lines();
		F.phase = P_WT;
		F.when = t + byte_time(side);
		break;

		case P_WT:
		if (F.drq)
		{
			F.status |= ST_LOST;
			F.drq = 0;
			F.buf[F.buflen++] = 0;
		}
		else
			F.buf[F.buflen++] = F.data;
		if (F.buflen < F.len)
		{
			F.drq = 1;
			lines();
			F.when = F.base + (uint64_t)F.buflen * byte_time(F.side);
		}
		else
		{
			F.drq = 0;
			lines();
			parse_format(F.len);
			finish(F.base + REV);
		}
		break;

		case P_END:
		finish(t);
		break;
	}
}

/* Brings the controller up to the present */
static void catch_up(void)
{
	uint64_t now = now_cycles();
	int guard = 0;

	while (F.phase != P_IDLE && F.when <= now && guard++ < 100000)
		act();
	if (!F.busy && F.motor && now >= F.motor_off)
	{
		F.motor = 0;
		F.spun = 0;
		save_discs();
	}
	if (F.index_irq && spinning() && now / REV != F.last_index)
	{
		F.last_index = now / REV;
		F.intrq = 1;
		lines();
	}
}

void FDC_Regular(ARMul_State *state)
{
	UNUSED_VAR(state);
	if (c711_on)
		c711_regular();
	else
		catch_up();
}

uint_fast8_t FDC_Read(ARMul_State *state, uint_fast16_t offset)
{
	uint8_t v;
	drive_t *d = sel();

	UNUSED_VAR(state);
	catch_up();
	switch ((offset >> 2) & 3)
	{
		case 0:
		if (F.type1)
		{
			v = F.status & (ST_CRC | ST_RNF);
			if (spinning() && now_cycles() % REV < INDEX_PULSE)
				v |= ST_INDEX;
			if (d && d->cyl == 0)
				v |= ST_TR00;
			if (F.spun)
				v |= ST_SPUN;
			if (d && d->disc && d->wp)
				v |= ST_WP;
		}
		else
			v = (F.status & (ST_LOST | ST_CRC | ST_RNF | 0x20 | ST_WP)) | (F.drq ? ST_DRQ : 0);
		v |= (F.busy ? ST_BUSY : 0) | (F.motor ? ST_MOTOR : 0);
		if (!F.force_latched && F.intrq)
		{
			F.intrq = 0;
			lines();
		}
		return v;

		case 1:
		return F.track;

		case 2:
		return F.sector;

		default:
		if (F.drq)
		{
			F.drq = 0;
			lines();
			if (fast && F.when > now_cycles() + FAST_GAP)  /* the next byte comes soon after */
				F.when = now_cycles() + FAST_GAP;
		}
		return F.data;
	}
}

/* Cycles until the controllers next want to be brought up to date: often during a fast read */
unsigned FDC_PollCycles(void)
{
	return fast || (c711_on && c711_fast()) ? 50 : 250;
}

void FDC_Write(ARMul_State *state, uint_fast16_t offset, uint_fast8_t data)
{
	UNUSED_VAR(state);
	catch_up();
	switch ((offset >> 2) & 3)
	{
		case 0:
		command((uint8_t)data, now_cycles());
		break;

		case 1:
		F.track = (uint8_t)data;
		break;

		case 2:
		F.sector = (uint8_t)data;
		break;

		default:
		F.data = (uint8_t)data;
		if (F.drq)
		{
			F.drq = 0;
			lines();
		}
		break;
	}
}

void FDC_LatchAChange(ARMul_State *state, uint_fast8_t data)
{
	int i;

	UNUSED_VAR(state);
	catch_up();
	F.latch_a = (uint8_t)data;
	F.cur = -1;
	for (i = 0; i < 4 && F.cur < 0; i++)
		if (!(data & (1 << i)))
			F.cur = i;
}

void FDC_LatchBChange(ARMul_State *state, uint_fast8_t data)
{
	int reset = !(data & 0x08);

	UNUSED_VAR(state);
	catch_up();
	F.latch_b = (uint8_t)data;
	F.sd = (data & 0x02) != 0;
	if (reset && !F.reset)                  /* master reset: whatever was going on stops */
	{
		F.busy = 0;
		F.phase = P_IDLE;
		F.intrq = F.drq = F.force_latched = F.index_irq = 0;
		F.status = 0;
		F.type1 = 1;
		F.sector = 1;
		lines();
	}
	F.reset = reset;
}

void FDC_Init(ARMul_State *state)
{
	UNUSED_VAR(state);
	free(F.buf);
	memset(&F, 0, sizeof F);
	F.latch_a = F.latch_b = 0xff;
	F.cur = -1;
	F.sd = 1;
	F.dir = 1;
	F.type1 = 1;
	F.sector = 1;
	F.motor_off = NEVER;
}

/* ---- Discs in and out ---- */

int fdc_insert(int drive, const char *path, int write_protect)
{
	disc_t *disc;
	drive_t *d;

	if (drive < 0 || drive > 3)
		return -1;
	catch_up();
	disc = disc_open(path);
	if (!disc)
		return -1;
	d = &floppy[drive];
	if (F.busy && F.cur == drive)           /* the disc went from under a command */
	{
		F.status |= ST_RNF;
		finish(now_cycles());
	}
	if (d->disc)
	{
		if (d->disc->changed && !d->wp)
			disc_save(d->disc);
		disc_free(d->disc);
	}
	d->disc = disc;
	d->wp = write_protect;
	d->changed = 1;
	return 0;
}

void fdc_close(void)
{
	int i;

	save_discs();
	for (i = 0; i < 4; i++)
	{
		disc_free(floppy[i].disc);
		floppy[i].disc = NULL;
	}
}

int fdc_motor(void)
{
	return c711_on ? c711_motor() : F.motor;
}

/* The rest of ArcEm's floppy interface, for a front end of ArcEm's own */
const char *FDC_InsertFloppy(uint_fast8_t drive, const char *image)
{
	return fdc_insert(drive, image, 0) ? "couldn't open disc image" : NULL;
}

const char *FDC_EjectFloppy(uint_fast8_t drive)
{
	if (drive < 4 && floppy[drive].disc)
	{
		if (floppy[drive].disc->changed && !floppy[drive].wp)
			disc_save(floppy[drive].disc);
		disc_free(floppy[drive].disc);
		floppy[drive].disc = NULL;
		floppy[drive].changed = 1;
	}
	return NULL;
}

bool FDC_IsFloppyInserted(uint_fast8_t drive)
{
	return drive < 4 && floppy[drive].disc;
}

void FDC_SetLEDsChangeFunc(void (*leds_changed)(uint_fast8_t leds))
{
	UNUSED_VAR(leds_changed);
}

void FDC_UpdateLEDs(void)
{
}

/* ---- Save states ---- */

/* No command is going, so nothing points into a disc */
int fdc_idle(void)
{
	return c711_on ? c711_idle() : !F.busy;
}

/* The name of the folder a disc image is in, which for a game's own discs is the game's id */
static void folder_name(const char *path, char *out, size_t size)
{
	int end = (int)(base_name(path) - path) - 1;
	char dir[1024];

	snprintf(dir, sizeof dir, "%.*s", end > 0 ? end : 0, path);
	snprintf(out, size, "%s", base_name(dir));
}

/* The image a state names, or one of the same name beside an image in the drives now, this drive's
   first. A game's discs all sit in its own folder, which may have moved since the state was saved,
   to another drive or with a portable copy. -1 if the state's discs are in another game's folder:
   a save of another game's, which isn't put back. */
static int local_image(const drive_t *d, const char *saved, char *out, size_t size)
{
	char was[256], now[256];
	int i;

	folder_name(saved, was, sizeof was);
	for (i = -1; i < 4; i++)
	{
		const disc_t *in = i < 0 ? d->disc : floppy[i].disc;
		FILE *f;

		if (!in)
			continue;
		folder_name(in->path, now, sizeof now);
		if (strcmp(was, now))
			return -1;
		snprintf(out, size, "%.*s%s", (int)(base_name(in->path) - in->path), in->path, base_name(saved));
		if ((f = fopen(out, "rb")))
		{
			fclose(f);
			return 0;
		}
	}
	snprintf(out, size, "%s", saved);
	return 0;
}

/* A drive's head, and the disc in it by its image and what has been written to it since the image
   was made. Reading one back puts that disc in again with just those writes, and they replace
   <image>.sav when it is next written: a disc with none has its .sav taken away now. */
static void drive_snap(snap_t *s, drive_t *d)
{
	char path[1024] = "";
	uint32_t has = d->disc != NULL, len = 0;
	uint8_t *changes = NULL;
	size_t n = 0;
	disc_t *disc;

	SNAP(s, has);
	SNAP(s, d->cyl);
	SNAP(s, d->wp);
	SNAP(s, d->changed);
	if (s->saving && has)
	{
		snprintf(path, sizeof path, "%s", d->disc->path);
		changes = disc_changes(d->disc, &n);
		len = (uint32_t)n;
	}
	if (has)
	{
		snap_bytes(s, path, sizeof path);
		SNAP(s, len);
		if (!s->saving && len > 64 << 20)
			s->bad = 1;
		if (!s->saving && !s->bad)
			changes = malloc(len ? len : 1);
		if (changes)
			snap_bytes(s, changes, len);
	}
	if (!s->saving && !s->bad)
	{
		char saved[1024];

		disc = NULL;
		path[sizeof path - 1] = 0;
		snprintf(saved, sizeof saved, "%s", path);
		if (has && local_image(d, saved, path, sizeof path))
			s->bad = 1;
		else if (has && !(disc = disc_open_plain(path)))
			s->bad = 1;
		else
		{
			disc_free(d->disc);
			d->disc = disc;
			if (disc && len)
				disc_apply(disc, changes, len);
			else if (disc && !d->wp)
			{
				char sav[1100];

				snprintf(sav, sizeof sav, "%s.sav", path);
				remove(sav);
			}
		}
	}
	free(changes);
}

/* The 1772's registers and lines, and the drives, taken with no command going. The buffer it
   reads and writes a track in is its own and stays. */
void fdc_snap(snap_t *s)
{
	uint8_t *buf = F.buf;
	int buflen = F.buflen, i;

	SNAP(s, F);
	if (!s->saving)
	{
		F.buf = buf;
		F.buflen = buflen;
		F.side = NULL;
		fast = 0;
	}
	for (i = 0; i < 4; i++)
		drive_snap(s, &floppy[i]);
}
