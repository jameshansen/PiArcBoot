/* PiArcBoot: save states, the whole machine written into a buffer in memory and read back from one.

   Each part of the machine has a function that does both, as saving says, so what is read back
   can't come in another order from what was written. A part that runs short or reads a value it
   can't take sets bad, and the state isn't used.

   Free software under the GNU General Public License, version 2 or (at your option) any later
   version. */
#ifndef SNAP_H
#define SNAP_H

#include <stddef.h>
#include <stdint.h>

typedef struct snap_s
{
	uint8_t *buf;
	size_t len, cap;        /* how much it holds, and room for */
	size_t at;              /* where reading has got to */
	int saving, bad;
} snap_t;

void snap_bytes(snap_t *s, void *p, size_t n);
#define SNAP(s, v) snap_bytes((s), &(v), sizeof(v))

#endif
