/* PiArcBoot: floppy discs as a drive sees them, a track side at a time.

   Free software under the GNU General Public License, version 2 or (at your option) any later
   version. */
#ifndef DISC_H
#define DISC_H

#include <stddef.h>
#include <stdint.h>

#define DISC_CYLS 88            /* cylinders a drive's head can reach */
#define DISC_DD_BYTES 6250      /* bytes in a revolution at double density: 250 kbit/s at 300 rpm */
#define DISC_HD_BYTES 12500     /* and at high density, 500 kbit/s */

enum { DENS_SINGLE = 1, DENS_DOUBLE = 2, DENS_HIGH = 4 };

/* A sector as it lies on the track: its ID field, its data, and where it passes the head */
typedef struct
{
	uint8_t c, h, r, n;     /* the ID field: track, side, sector and size code */
	uint8_t id_crc;         /* the ID field reads with a CRC error */
	uint8_t data_crc;       /* the data field reads with a CRC error */
	uint8_t density;        /* DENS_ */
	int pos;                /* the ID address mark, in bytes from the index pulse; -1 until laid out */
	uint32_t len;           /* bytes of data */
	uint8_t *data;
} dsector_t;

/* One side of one track */
typedef struct
{
	int n;
	dsector_t *s;           /* in the order they pass the head once laid out */
	int bytes;              /* bytes in a revolution at the side's density */
	int changed;            /* written to since the image was loaded */
} dside_t;

typedef struct
{
	char path[1024];        /* the image; what is written to it goes to <path>.sav */
	int cyls;               /* cylinders the image has anything on */
	dside_t side[DISC_CYLS][2];
	int changed;
} disc_t;

/* A drive, which ever floppy controller the machine has works: the 1772 (wd1772.c) or the
   82C711 (82c711.c) */
typedef struct
{
	disc_t *disc;
	int cyl;                /* where the head is */
	int wp;
	int changed;            /* a disc has gone in since the head last stepped: the PC drive's disc changed line */
} drive_t;

extern drive_t floppy[4];

disc_t *disc_open(const char *path);                    /* a JFD, HFE or flat image, with its saved changes; NULL if it can't be read */
disc_t *disc_parse(const uint8_t *d, size_t len);       /* the same from memory, with no saved changes */
void disc_free(disc_t *d);
int disc_save(disc_t *d);                               /* writes <path>.sav if anything has changed; 0 when done */
void disc_set_side(disc_t *d, int cyl, int head, dsector_t *s, int n, int bytes);       /* a side made afresh, as a format does */
void disc_layout(dside_t *side);                        /* gives the sectors with no place on the track one, and puts them in track order */
uint16_t disc_crc(uint16_t crc, const uint8_t *p, size_t n);   /* the CRC the 1772 checks, CCITT, from &FFFF */
uint16_t disc_id_crc(const dsector_t *s);               /* the CRC of a sector's ID field, address marks included */
uint16_t disc_data_crc(const uint8_t *d, size_t n, uint8_t mark);       /* the CRC of a data field */
int disc_track_bytes(const dside_t *side, uint8_t *out, uint8_t *sync); /* the side as the bytes a Read Track gives; sync marks A1s written with a missing clock */
int disc_selftest(void);

uint8_t *jfd_read(const char *path, size_t *len);
uint8_t *jfd_flatten(const char *path, size_t *len);

#endif
