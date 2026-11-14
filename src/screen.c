/* PiArcBoot: the picture. ArcEm's standard display driver (arch/stddisplaydev.c) draws the
   VIDC's display area into a buffer of its own a line at a time, as the beam would. At the start
   of each frame the last whole frame is copied to emu_fb, with the pointer drawn over it as the
   VIDC lays its cursor over the picture.

   A mode under 512 pixels across has each pixel drawn twice, so the usual game modes come out
   640 across, and a mode at least twice as wide as it is tall has emu_dbl set for the game
   window to show each line twice. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../arcem/armdefs.h"
#include "../arcem/arch/ArcemConfig.h"
#include "../arcem/arch/armarc.h"
#include "../arcem/arch/archio.h"
#include "../arcem/eventq.h"
#include "../arcem/arch/dbugsys.h"
#include "../arcem/arch/displaydev.h"
#include "../arcem/arch/ControlPane.h"
#include "piarcboot.h"
#include "core.h"

#define FB_ROWS 1024            /* the VIDC's vertical registers count to 1023 */

uint32_t *emu_fb;
int emu_w = 640, emu_h = 256, emu_dbl = 1;
unsigned emu_frames;
int machine_mode_sets;

static uint32_t *back;          /* where the driver draws */
static int back_w, back_h, xscale = 1;

/* A 13 bit VIDC colour, of which the picture uses the 12 of red, green and blue, as &00RRGGBB */
static uint32_t colour(unsigned col)
{
	return (col & 15) * 0x110000u | (col >> 4 & 15) * 0x1100u | (col >> 8 & 15) * 0x11u;
}

void screen_alloc(void)
{
	if (!emu_fb)
	{
		emu_fb = calloc((size_t)EMU_FB_W * FB_ROWS, 4);
		back = calloc((size_t)EMU_FB_W * FB_ROWS, 4);
	}
}

/* ---- An instance of ArcEm's standard display driver, drawing 32 bits a pixel ---- */

#define SDD_HostColour uint32_t
#define SDD_Name(x) pab_##x
#define SDD_RowsAtOnce 1
#define SDD_Row SDD_HostColour *
#define SDD_DisplayDev PAB_DisplayDev

static SDD_HostColour SDD_Name(Host_GetColour)(ARMul_State *state, unsigned int col)
{
	UNUSED_VAR(state);
	return colour(col);
}

static bool SDD_Name(Host_ChangeMode)(ARMul_State *state, int width, int height, int hz);

static inline SDD_Row SDD_Name(Host_BeginRow)(ARMul_State *state, int row, int offset)
{
	UNUSED_VAR(state);
	return back + (size_t)row * EMU_FB_W + offset;
}

static inline void SDD_Name(Host_EndRow)(ARMul_State *state, SDD_Row *row)
{
	UNUSED_VAR(state);
	UNUSED_VAR(row);
}

static inline void SDD_Name(Host_BeginUpdate)(ARMul_State *state, SDD_Row *row, unsigned int count)
{
	UNUSED_VAR(state);
	UNUSED_VAR(row);
	UNUSED_VAR(count);
}

static inline void SDD_Name(Host_EndUpdate)(ARMul_State *state, SDD_Row *row)
{
	UNUSED_VAR(state);
	UNUSED_VAR(row);
}

static inline void SDD_Name(Host_SkipPixels)(ARMul_State *state, SDD_Row *row, unsigned int count)
{
	UNUSED_VAR(state);
	(*row) += count;
}

static inline void SDD_Name(Host_WritePixel)(ARMul_State *state, SDD_Row *row, SDD_HostColour pix)
{
	UNUSED_VAR(state);
	*(*row)++ = pix;
}

static inline void SDD_Name(Host_WritePixels)(ARMul_State *state, SDD_Row *row, SDD_HostColour pix, unsigned int count)
{
	UNUSED_VAR(state);
	while (count--)
		*(*row)++ = pix;
}

static void SDD_Name(Host_PollDisplay)(ARMul_State *state);

#include "../arcem/arch/stddisplaydev.c"

static bool SDD_Name(Host_ChangeMode)(ARMul_State *state, int width, int height, int hz)
{
	int xs = width < 512 ? 2 : 1;

	UNUSED_VAR(hz);
	if (width * xs > EMU_FB_W || height > FB_ROWS)
	{
		ControlPane_Error(false, "a mode of %d by %d is more than the picture can hold", width, height);
		return false;
	}
	back_w = width * xs;
	back_h = height;
	xscale = xs;
	HD.XScale = xs;
	HD.YScale = 1;
	HD.Width = back_w;
	HD.Height = back_h;
	memset(back, 0, (size_t)EMU_FB_W * FB_ROWS * 4);
	return true;
}

/* The pointer over the picture: 32 pixels of 2 bits a line from Vert_CursorStart to
   Vert_CursorEnd, read from the address in MEMC's Cinit register. Colour 0 lets the picture
   through and 1 to 3 are the cursor's own palette. */
static void draw_pointer(ARMul_State *state)
{
	int height = (int)VIDC.Vert_CursorEnd - (int)VIDC.Vert_CursorStart, px, py, x, y, s;
	uint32_t at = (uint32_t)MEMC.Cinit * 16;

	if (height <= 0)
		return;
	DisplayDev_GetCursorPos(state, &px, &py);
	for (y = 0; y < height; y++, at += 8)
	{
		ARMword w[2];

		if (py + y < 0 || py + y >= emu_h)
			continue;
		if (at + 8 > MEMC.RAMSize)
			break;
		w[0] = MEMC.PhysRam[at / 4];
		w[1] = MEMC.PhysRam[at / 4 + 1];
		for (x = 0; x < 32; x++)
		{
			unsigned pix = w[x / 16] >> ((x & 15) * 2) & 3;

			if (!pix)
				continue;
			for (s = 0; s < xscale; s++)
			{
				int hx = (px + x) * xscale + s;

				if (hx >= 0 && hx < emu_w)
					emu_fb[(size_t)(py + y) * EMU_FB_W + hx] = colour(VIDC.CursorPalette[pix - 1]);
			}
		}
	}
}

/* The start of a frame: the one before it is whole, and goes out */
static void SDD_Name(Host_PollDisplay)(ARMul_State *state)
{
	int y;

	if (back_w > 0 && back_h > 0)
	{
		emu_w = back_w;
		emu_h = back_h;
		emu_dbl = back_w >= back_h * 2;
		for (y = 0; y < back_h; y++)
			memcpy(emu_fb + (size_t)y * EMU_FB_W, back + (size_t)y * EMU_FB_W, (size_t)back_w * 4);
		draw_pointer(state);
	}
	emu_frames++;
}

/* Writes to the VIDC, counting those to the horizontal cycle register (&80) that set up a mode */
static void vidc_write(ARMul_State *state, ARMword address, ARMword data, bool bNw)
{
	if ((data >> 24 & 0xfc) == 0x80)
		machine_mode_sets++;
	SDD_Name(VIDCPutVal)(state, address, data, bNw);
}

static const DisplayDev screen_dev = {
	SDD_Name(Init),
	SDD_Name(Shutdown),
	vidc_write,
	SDD_Name(DAGWrite),
	SDD_Name(IOEBCRWrite),
};

bool DisplayDev_Init(ARMul_State *state)
{
	screen_alloc();
	return DisplayDev_Set(state, &screen_dev);
}

void emu_save_shot(const char *fn)
{
	uint8_t *rgba = malloc((size_t)emu_w * emu_h * 4);
	int x, y;

	if (!rgba || !emu_fb)
	{
		free(rgba);
		return;
	}
	for (y = 0; y < emu_h; y++)
		for (x = 0; x < emu_w; x++)
		{
			uint32_t p = emu_fb[(size_t)y * EMU_FB_W + x];
			uint8_t *o = rgba + ((size_t)y * emu_w + x) * 4;

			o[0] = (uint8_t)(p >> 16);
			o[1] = (uint8_t)(p >> 8);
			o[2] = (uint8_t)p;
			o[3] = 255;
		}
	if (png_write(fn, rgba, emu_w, emu_h))
		fprintf(stderr, "piarcboot: can't write %s\n", fn);
	free(rgba);
}
