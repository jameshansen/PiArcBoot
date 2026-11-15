/* PiArcBoot: how the parts of the machine's layer over ArcEm reach one another. */
#ifndef CORE_H
#define CORE_H

#include <stdint.h>

#define MACHINE_HZ 25000000             /* ArcEm cycles in a second of machine time: 25 MHz for an ARM3 */
#define MACHINE_CS (MACHINE_HZ / 100)   /* and in a centisecond */

struct snap_s;

/* emu.c */
uint64_t machine_clock(void);           /* cycles since power on */

/* wd1772.c: the floppy controller and its drives */
int fdc_insert(int drive, const char *path, int write_protect);
void fdc_close(void);                   /* writes back what was saved to the discs, and lets them go */
int fdc_motor(void);                    /* the floppy controller's motor line */
int fdc_idle(void);                     /* no command going, on whichever controller the machine has */
void fdc_snap(struct snap_s *s);        /* for a save state: the 1772 and the drives */

/* 82c711.c: the A5000's IOEB and 82C711, in place of the 1772 */
extern int c711_on;
void c711_start(int on);                /* before power on: whether the machine has them */
void c711_regular(void);
int c711_motor(void);
int c711_idle(void);
void c711_snap(struct snap_s *s);

/* screen.c */
void screen_alloc(void);
int screen_xscale(void);                /* 2 when each of the mode's pixels is drawn twice across, or 1 */

#endif
