/* The game window: one game, booted hands-free and shown in a window with the system's menu bar.

   A game starts behind a splash screen. The machine is run as fast as the host allows while a
   script holds Shift for a disc that boots, or ROO311's HostLink module runs the game's
   application for one that doesn't, and the splash gives way to the picture once the game has
   put up a screen of its own, so the desktop is never seen. From there the machine runs in real
   time.

   The Game menu has Pause, full screen and the three window sizes. Saves has three slots, each a whole
   machine kept in a file: one of them is written every 30 seconds of play, and restoring a slot
   makes it that one. Switch Floppy, there only for a game with more than one disc, changes the
   disc in the drive, and an auto switch point remembers the screen a game asks for a disc on, so
   that the next time the game shows that screen the disc changes by itself. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <io.h>
#include <share.h>
#else
#include <sys/file.h>
#endif
#include <SDL.h>
#include "piarcboot.h"
#include "default_cmos.h"

#define GRID_W 80
#define GRID_H 64
#define GRID (GRID_W * GRID_H)
#define MAX_SWAPS 16

typedef struct
{
	int disc;                       /* the disc to put in, counting from 0 */
	uint8_t sig[GRID];              /* the screen that asks for it */
} swap_t;

static game_t game;
static int cur_disc;
static SDL_Texture *pic, *big, *icon_tex;        /* the picture, and the picture enlarged a whole number of times */
static int pic_w, pic_h, big_w, big_h;
static int fullscreen, captured, told_capture;
static int crop, crop_fitted;           /* just the windows on the screen are shown; the window has been fitted to them */
static int crop_from, now_cs;           /* the centisecond cropping starts, a second after the game is going; the machine's now */
static int native_bar;                  /* the window has a menu bar of the system's own making */
static int fit_w, fit_h;                /* the size the window was last given for a size in the menu */
static int kept_w, kept_h;              /* and its size before it went full screen */
static int boxed;                       /* the whole screen is shown, uncropped, for an error box the game has open */

/* The shaders the Shaders menu has, by their presets beside the program */
static const struct { const char *name, *preset; } shaders[] = {
	{ "CRT-Royale-Pi", "shaders/crt/crt-royale-pi.glslp" },
	{ "CRT-Pi", "shaders/crt/crt-pi.glslp" },
};
#define NSHADERS (int)(sizeof shaders / sizeof *shaders)
static settings_t settings;             /* the scale and the shader, the same for every game */
static int can_shade, shader_on;        /* the renderer is one the shaders run in; the chosen one is loaded and drawing */

#define MENUS 3
#define SLOTS 3
#define MAX_ITEMS (MAX_DISCS + 14 + SLOTS * 4 + NSHADERS)
static const char *const menu_titles[MENUS] = { "Game", "Saves", "Switch Floppy" };
static int n_menus = MENUS;             /* 2 for a game with one disc: no Switch Floppy */
enum { CMD_FULLSCREEN = 1, CMD_SCALE, CMD_CLOSE = CMD_SCALE + 4, CMD_SET_SWAP, CMD_CLEAR_SWAPS, CMD_SEND_ESC, CMD_CROP, CMD_DISC,
	CMD_AUTO = 100, CMD_RESTORE = CMD_AUTO + SLOTS, CMD_SAVE = CMD_RESTORE + SLOTS, CMD_SELECTOR = CMD_SAVE + SLOTS, CMD_SNAPSHOT,
	CMD_PAUSE, CMD_SHADER };
static time_t saved_at[SLOTS];          /* when each slot was last written, or 0 for an empty one */
static int armed;                       /* Auto Save may write its slot: it was empty, or since the game started it has been restored or saved to, or a slot chosen */
static int want_save, want_restore;     /* a slot to save to or restore, counting from 1, waiting for the floppy drive to finish */
static int writing_slot = -1;           /* the slot being written on its own thread */
static int saving_note;                 /* it was the player's Save, which is to say so when it's written */
static int selector;                    /* the selector is open: the slots side by side, and the game stopped behind them */
static int want_new;                    /* New Game was chosen for this slot, counting from 1 */
static int erasing;                     /* the slot whose save the selector is asking to erase, counting from 1 */
static int confirming;                  /* the slot whose save Save is asking to write over, counting from 1 */
static int paused;
static int played;                      /* the game has run since it started, or was restored, so a new game starts it again */
static uint8_t *power_on;               /* the machine as it was at power on, which a new game goes back to */
static size_t power_on_len;
static SDL_Texture *slot_tex[SLOTS];    /* each slot's screenshot, as the selector opened */
static SDL_Texture *blur_tex;           /* a restored game's picture blurred, behind the splash while it loads */
#define FADE_MS 600                     /* how long that picture takes to clear before the game runs */
static int slot_w[SLOTS], slot_h[SLOTS];
static char note[160];
static Uint32 note_until;
static swap_t swaps[MAX_SWAPS];
static int n_swaps, swap_pending, swap_hits[MAX_SWAPS], swap_armed[MAX_SWAPS];
static uint8_t pending_sig[GRID];
static double mouse_fx, mouse_fy;

static void notify(const char *text)
{
	snprintf(note, sizeof note, "%s", text);
	note_until = SDL_GetTicks() + 3500;
}

/* The game is held still, and nothing typed or clicked reaches it: the selector is open, or a box
   asking about a save, or it is paused */
static int stopped(void)
{
	return selector || confirming || paused;
}

/* The picture as 80 by 64 brightnesses, for telling one screen from another */
static void signature(uint8_t *sig)
{
	int gx, gy, x, y;

	for (gy = 0; gy < GRID_H; gy++)
		for (gx = 0; gx < GRID_W; gx++)
		{
			int x0 = gx * emu_w / GRID_W, x1 = (gx + 1) * emu_w / GRID_W;
			int y0 = gy * emu_h / GRID_H, y1 = (gy + 1) * emu_h / GRID_H;
			unsigned sum = 0, n = 0;

			for (y = y0; y < y1; y++)
				for (x = x0; x < x1; x++)
				{
					uint32_t p = emu_fb[(size_t)y * EMU_FB_W + x];

					sum += (p >> 16 & 255) * 2 + (p >> 8 & 255) * 5 + (p & 255);
					n += 8;
				}
			sig[gy * GRID_W + gx] = n ? sum / n : 0;
		}
}

/* How many of the 5120 cells differ between two screens */
static int sig_diff(const uint8_t *a, const uint8_t *b)
{
	int i, n = 0;

	for (i = 0; i < GRID; i++)
		n += abs(a[i] - b[i]) > 24;
	return n;
}

static void swaps_file(char *path, size_t size)
{
	store_game_path(game.id, "swaps.bin", path, size);
}

static void swaps_load(void)
{
	char path[1024];
	FILE *f;

	swaps_file(path, sizeof path);
	f = fopen(path, "rb");
	if (!f)
		return;
	while (n_swaps < MAX_SWAPS && fread(&swaps[n_swaps], sizeof(swap_t), 1, f) == 1)
		if (swaps[n_swaps].disc >= 0 && swaps[n_swaps].disc < game.ndiscs)
			swap_armed[n_swaps++] = 1;
	fclose(f);
}

static void swaps_save(void)
{
	char path[1024];

	swaps_file(path, sizeof path);
	if (n_swaps)
		file_write(path, swaps, n_swaps * sizeof(swap_t));
	else
		remove(path);
}

/* The number a disk goes by: the one in its file's name (disc4.jfd is Disk 4), which for a
   JASPP game is its number in the set even when the set has discs missing */
static int disk_number(int n)
{
	int number = 0;

	if (!game.jaspp && !game.classic)       /* one of the player's own, whose discs they can put in any order */
		return n + 1;
	return sscanf(game.disc[n], "disc%d", &number) == 1 && number > 0 ? number : n + 1;
}

static void insert_disc(int n, const char *how)
{
	char path[1024], text[160];

	if (n < 0 || n >= game.ndiscs)
		return;
	store_game_path(game.id, game.disc[n], path, sizeof path);
	machine_disc(0, path, game.ro[n]);
	cur_disc = n;
	if (getenv("PIARCBOOT_TRACE"))
		fprintf(stderr, "disk %d %s%c", disk_number(n), how ? how : "inserted", 10);
	if (how)
	{
		snprintf(text, sizeof text, "Disk %d %s", disk_number(n), how);
		notify(text);
	}
}

/* Picks a disc by hand. With an auto switch point waiting for its disc, this is the one. */
static void choose_disc(int n)
{
	if (swap_pending && n_swaps < MAX_SWAPS)
	{
		swaps[n_swaps].disc = n;
		memcpy(swaps[n_swaps].sig, pending_sig, GRID);
		swap_armed[n_swaps] = 0;        /* not until the game has left this screen */
		n_swaps++;
		swaps_save();
		insert_disc(n, "inserted, and it will be whenever this screen comes back");
	}
	else
		insert_disc(n, "inserted");
	swap_pending = 0;
}

/* Looks at the picture 10 times a second for a screen that an auto switch point was set on.
   The screen has to hold still for half a second, and has to go away before it counts again. */
static void swaps_check(void)
{
	uint8_t sig[GRID];
	int i;

	if (!n_swaps)
		return;
	signature(sig);
	for (i = 0; i < n_swaps; i++)
	{
		if (getenv("PIARCBOOT_TRACE") && sig_diff(sig, swaps[i].sig) < 400)
			fprintf(stderr, "switch point %d: %d cells differ%c", i, sig_diff(sig, swaps[i].sig), 10);
		if (sig_diff(sig, swaps[i].sig) > 1)
		{
			swap_hits[i] = 0;
			swap_armed[i] = 1;
		}
		else if (swap_armed[i] && ++swap_hits[i] >= 5)
		{
			swap_armed[i] = 0;
			if (swaps[i].disc != cur_disc)
				insert_disc(swaps[i].disc, "inserted automatically");
		}
	}
}

/* How much of the top of the window the menu bar takes, in real pixels */
static int bar_px(void)
{
	return native_bar ? menubar_height() : 0;
}

/* How many real pixels each way a pixel of the 1x picture gets on this display: the display's
   scale as a whole number, so that the sizes in the menu keep every pixel the same size */
static int pic_scale(void)
{
	return rg_scale < 1.5f ? 1 : (int)(rg_scale + 0.5f);
}

/* The part of the picture shown, in its pixels: all of it, or for a game cropped to its windows,
   the least that holds every window on the screen, as ROO311's HostLink says where they are,
   leaving out the desktop's backdrop. That starts once the game is going and the folder it was
   started from has had time to close. A window wholly off the screen is hidden, so it is left
   out too. With no windows the picture is shown whole, and so it is while the game has an error
   box open, which may be outside its windows. */
static SDL_Rect view(void)
{
	SDL_Rect v = { 0, 0, emu_w, emu_h };
	int ys = emu_dbl ? 4 : 2, sh = emu_h * ys, x0 = 1280, y0 = sh, x1 = 0, y1 = 0, i;

	for (i = 0; crop && !emu_host_error_open && crop_from && now_cs >= crop_from && i < emu_host_nwins; i++)
	{
		const int *w = emu_host_wins[i];
		int a = w[0] > 0 ? w[0] : 0, b = w[1] > 0 ? w[1] : 0, c = w[2] < 1280 ? w[2] : 1280, d = w[3] < sh ? w[3] : sh;

		if (c <= a || d <= b)
			continue;
		x0 = a < x0 ? a : x0;
		y0 = b < y0 ? b : y0;
		x1 = c > x1 ? c : x1;
		y1 = d > y1 ? d : y1;
	}
	if (x1 - x0 < 32 || y1 - y0 < 32)
		return v;
	v.x = x0 * emu_w / 1280;
	v.w = (x1 * emu_w + 1279) / 1280 - v.x;
	v.y = emu_h - (y1 + ys - 1) / ys;
	v.h = emu_h - y0 / ys - v.y;
	return v;
}

/* The picture's size in real pixels for a scale of 1 to 3 */
static void picture_size(int scale, int *w, int *h)
{
	SDL_Rect v = view();
	int dw = emu_w > 0 ? v.w : 640, dh = emu_h > 0 ? (emu_dbl ? v.h * 2 : v.h) : 512;

	*w = dw * scale / 2 * pic_scale();
	*h = dh * scale / 2 * pic_scale();
}

/* The window's size in real pixels for the menu's scale: the picture, with the menu bar above it */
static void window_size(int *w, int *h)
{
	picture_size(settings.scale, w, h);
	*h += bar_px();
}

/* Whether the window is at a size it was dragged to, and not the last one from the menu */
static int own_size(void)
{
	int w, h;

	SDL_GetWindowSize(rg_win, &w, &h);
	return w != fit_w || h != fit_h;
}

/* The window's size in its own units for a picture that size in real pixels, and no smaller than
   it is allowed to be, so that it is the size it is given */
static void window_units(int *w, int *h)
{
	int min_w = 320 * pic_scale(), min_h = 200 * pic_scale() + bar_px();

	*w = rg_to_window(*w > min_w ? *w : min_w);
	*h = rg_to_window(*h > min_h ? *h : min_h);
}

/* Gives the window the size the menu's scale asks for */
static void fit_window(void)
{
	window_size(&fit_w, &fit_h);
	window_units(&fit_w, &fit_h);
	SDL_SetWindowSize(rg_win, fit_w, fit_h);
}

/* For a game cropped to its windows, a window made bigger at the menu's scale when the game's
   windows have outgrown it. It never shrinks, so a menu or a box that comes and goes leaves it
   as it was. */
static void grow_window(void)
{
	int w, h;

	window_size(&w, &h);
	window_units(&w, &h);
	if (w <= fit_w && h <= fit_h)
		return;
	fit_w = w > fit_w ? w : fit_w;
	fit_h = h > fit_h ? h : fit_h;
	SDL_SetWindowSize(rg_win, fit_w, fit_h);
}

/* Full screen has no menu bar, and Ctrl+Alt+Enter comes back. Coming back, the window is the size
   it was. */
static void set_fullscreen(int on)
{
	if (on == fullscreen)
		return;
	fullscreen = on;
	if (on)
	{
		SDL_GetWindowSize(rg_win, &kept_w, &kept_h);
		if (native_bar)
			menubar_show(0);
		SDL_SetWindowFullscreen(rg_win, SDL_WINDOW_FULLSCREEN_DESKTOP);
	}
	else
	{
		SDL_SetWindowFullscreen(rg_win, 0);
		if (native_bar)
			menubar_show(1);
		SDL_SetWindowSize(rg_win, kept_w, kept_h);
	}
}

/* The game has the mouse, and the keyboard with it: the system's own keys, such as Alt+F4 and
   Alt+Tab, go to the game while it has them, and Escape gives both back */
static void set_capture(int on)
{
	captured = on;
	SDL_SetRelativeMouseMode(on ? SDL_TRUE : SDL_FALSE);
	SDL_SetHint(SDL_HINT_WINDOWS_NO_CLOSE_ON_ALT_F4, on ? "1" : "0");
	SDL_SetWindowKeyboardGrab(rg_win, on ? SDL_TRUE : SDL_FALSE);
	if (on && !told_capture)
	{
		notify("Press ESC to release mouse");
		told_capture = 1;
	}
}

/* Where the picture goes, in real pixels: the largest that keeps its shape in the window below
   the menu bar, or on the whole screen when full screen, with black around it. Full screen keeps
   to a whole multiple of the 1x size, and so does a window within a hair of one. A picture cropped
   to the game's windows keeps its pixels no bigger than the whole picture's would be there, or
   than the menu's scale makes them, so a small window of the game's isn't blown up. */
static void picture_rect(SDL_Rect *r, double *zoom)
{
	SDL_Rect v = view();
	int ow, oh, top, dw = v.w, dh = emu_dbl ? v.h * 2 : v.h, fh = emu_dbl ? emu_h * 2 : emu_h, k;
	double most;

	SDL_GetRendererOutputSize(rg_ren, &ow, &oh);
	top = fullscreen ? 0 : bar_px();
	oh -= top;
	r->w = ow;
	r->h = (int)((Sint64)ow * dh / dw);
	if (r->h > oh)
	{
		r->h = oh;
		r->w = (int)((Sint64)oh * dw / dh);
	}
	if (v.w < emu_w || v.h < emu_h)
	{
		most = (double)ow / emu_w < (double)oh / fh ? (double)ow / emu_w : (double)oh / fh;
		if (most < settings.scale * pic_scale() / 2.0)
			most = settings.scale * pic_scale() / 2.0;
		if (r->w > dw * most)
		{
			r->w = (int)(dw * most);
			r->h = (int)(dh * most);
		}
	}
	k = r->w * 2 / dw;
	if (k >= 1 && (fullscreen || r->w - dw * k / 2 < 8))
	{
		r->w = dw * k / 2;
		r->h = dh * k / 2;
	}
	*zoom = (double)r->w / dw;
	r->x = (ow - r->w) / 2;
	r->y = top + (oh - r->h) / 2;
}

/* Draws the picture at whatever size the window gives it. The graphics chip does the scaling:
   the picture goes to it as it is and comes out at the size of its place in the window.

   Where every pixel of the picture gets a whole number of screen pixels, they are repeated. At
   a size in between, the picture is first enlarged that way a whole number of times, into a
   texture of its own, and that is then drawn at the window's size with the chip's smoothing.
   Only the screen pixels that straddle 2 of the picture's come out blended, so every pixel
   keeps an even width and flat areas stay crisp, where repeating alone gives narrow and wide
   columns and smoothing alone blurs everything.

   With a shader on, the shader draws it instead. */
static void draw_picture(void)
{
	SDL_Rect all = { 0, 0, emu_w, emu_h }, src = view(), dst;
	SDL_RendererInfo info;
	double zoom;
	int tx, ty;

	if (emu_w <= 0 || emu_h <= 0)
		return;
	if (!pic || pic_w != emu_w || pic_h != emu_h)
	{
		if (pic)
			SDL_DestroyTexture(pic);
		pic = SDL_CreateTexture(rg_ren, SDL_PIXELFORMAT_RGB888, SDL_TEXTUREACCESS_STREAMING, emu_w, emu_h);
		pic_w = emu_w;
		pic_h = emu_h;
		if (!fullscreen && !own_size())
			fit_window();           /* the game changed screen mode, and the window follows */
	}
	if (crop && boxed != emu_host_error_open)
	{
		boxed = emu_host_error_open;    /* the window takes the whole screen's shape for an error box, and the windows' again after */
		crop_fitted = 0;
		if (!fullscreen && !own_size())
			fit_window();
	}
	if (crop && (src.w < emu_w || src.h < emu_h) && !fullscreen && !own_size())
	{
		if (!crop_fitted)
			fit_window();           /* the window takes the shape of the game's windows when they first come */
		else
			grow_window();
		crop_fitted = 1;
	}
	SDL_UpdateTexture(pic, &all, emu_fb, EMU_FB_W * 4);
	picture_rect(&dst, &zoom);
	if (dst.w < 1 || dst.h < 1)
		return;
	if (shader_on)          /* the shader draws the picture at 2x Scale's size, whatever the window's, and that is drawn to fit */
	{
		SDL_Texture *t;
		int sw, sh, drawn = 0;

		picture_size(2, &sw, &sh);
		t = shader_run(pic, &src, sw, sh, &dst, &drawn);
		if (drawn)
			return;
		if (t)
		{
			SDL_RenderCopy(rg_ren, t, NULL, &dst);
			return;
		}
	}
	SDL_SetTextureScaleMode(pic, SDL_ScaleModeNearest);
	if (dst.w % src.w == 0 && dst.h % src.h == 0)
	{
		SDL_RenderCopy(rg_ren, pic, &src, &dst);
		return;
	}

	/* How many whole times larger to make it first, each way: as many as fit in its place
	   and in a texture */
	tx = dst.w / src.w > 1 ? dst.w / src.w : 1;
	ty = dst.h / src.h > 1 ? dst.h / src.h : 1;
	if (!SDL_GetRendererInfo(rg_ren, &info) && info.max_texture_width > 0 && info.max_texture_height > 0)
	{
		while (tx > 1 && src.w * tx > info.max_texture_width)
			tx--;
		while (ty > 1 && src.h * ty > info.max_texture_height)
			ty--;
	}
	if (tx > 1 || ty > 1)
	{
		if (!big || big_w != src.w * tx || big_h != src.h * ty)
		{
			if (big)
				SDL_DestroyTexture(big);
			big_w = src.w * tx;
			big_h = src.h * ty;
			big = SDL_CreateTexture(rg_ren, SDL_PIXELFORMAT_RGB888, SDL_TEXTUREACCESS_TARGET, big_w, big_h);
			if (big)
				SDL_SetTextureScaleMode(big, SDL_ScaleModeLinear);
		}
		if (big && !SDL_SetRenderTarget(rg_ren, big))
		{
			SDL_RenderCopy(rg_ren, pic, &src, NULL);
			SDL_SetRenderTarget(rg_ren, NULL);
			SDL_RenderCopy(rg_ren, big, NULL, &dst);
			return;
		}
	}
	SDL_SetTextureScaleMode(pic, SDL_ScaleModeLinear);      /* smaller than twice its size: smoothing alone */
	SDL_RenderCopy(rg_ren, pic, &src, &dst);
}

/* ---- Saves ---- */

static void slot_path(int k, char *path, size_t size)
{
	char name[40];

	snprintf(name, sizeof name, "saves/slot%d.state", k + 1);
	store_game_path(game.id, name, path, size);
}

static time_t slot_time(int k)
{
	char path[1024];
	struct stat st;

	slot_path(k, path, sizeof path);
	return stat(path, &st) ? 0 : st.st_mtime;
}

/* How long ago a slot was written, as the menu says it */
static void saved_when(time_t t, char *out, size_t size)
{
	long s = (long)difftime(time(NULL), t);

	if (s < 60)
		snprintf(out, size, "just now");
	else if (s < 3600)
		snprintf(out, size, "%ld min ago", s / 60);
	else if (s < 7200)
		snprintf(out, size, "1 hour ago");
	else if (s < 86400)
		snprintf(out, size, "%ld hours ago", s / 3600);
	else if (s < 2 * 86400)
		snprintf(out, size, "yesterday");
	else
		snprintf(out, size, "%ld days ago", s / 86400);
}

/* The picture as the player sees it, for a slot's screenshot: the part the window shows, its lines
   doubled where the window doubles them, each pixel the average of those it stands for, no bigger
   than 320 by 256. NULL with no picture yet. */
static uint32_t *screenshot(int *sw, int *sh)
{
	SDL_Rect v = view();
	int ys = emu_dbl ? 2 : 1, dh = v.h * ys, tw, th, tx, ty, x, y;
	double k;
	uint32_t *out;

	if (emu_w <= 0 || emu_h <= 0 || v.w <= 0 || v.h <= 0)
		return NULL;
	k = 320.0 / v.w < 256.0 / dh ? 320.0 / v.w : 256.0 / dh;
	k = k > 1 ? 1 : k;
	tw = (int)(v.w * k) > 0 ? (int)(v.w * k) : 1;
	th = (int)(dh * k) > 0 ? (int)(dh * k) : 1;
	if (!(out = malloc((size_t)tw * th * 4)))
		return NULL;
	for (ty = 0; ty < th; ty++)
		for (tx = 0; tx < tw; tx++)
		{
			int x0 = tx * v.w / tw, x1 = (tx + 1) * v.w / tw, y0 = ty * dh / th, y1 = (ty + 1) * dh / th;
			unsigned r = 0, g = 0, b = 0, n = 0;

			x1 = x1 > x0 ? x1 : x0 + 1;
			y1 = y1 > y0 ? y1 : y0 + 1;
			for (y = y0; y < y1; y++)
				for (x = x0; x < x1; x++)
				{
					uint32_t p = emu_fb[(size_t)(v.y + y / ys) * EMU_FB_W + v.x + x];

					r += p >> 16 & 255;
					g += p >> 8 & 255;
					b += p & 255;
					n++;
				}
			out[(size_t)ty * tw + tx] = (r / n) << 16 | (g / n) << 8 | b / n;
		}
	*sw = tw;
	*sh = th;
	return out;
}

/* Save Snapshot: the picture as the window shows it, the part in view with its lines doubled where
   they are, at its full size, as a PNG in the Pictures folder named after the game and the time,
   with " 2", " 3" and so on after a name already taken in the same second */
static void save_snapshot(void)
{
	SDL_Rect v = view();
	int ys = emu_dbl ? 2 : 1, h = v.h * ys, x, y, k;
	char dir[1024], path[1400], name[160], stamp[32], text[200];
	time_t now = time(NULL);
	struct stat st;
	uint8_t *rgba;
	size_t i;

	if (emu_w <= 0 || v.w <= 0 || h <= 0 || !(rgba = malloc((size_t)v.w * h * 4)))
		return;
	for (y = 0; y < h; y++)
		for (x = 0; x < v.w; x++)
		{
			uint32_t p = emu_fb[(size_t)(v.y + y / ys) * EMU_FB_W + v.x + x];
			uint8_t *o = rgba + ((size_t)y * v.w + x) * 4;

			o[0] = (uint8_t)(p >> 16);
			o[1] = (uint8_t)(p >> 8);
			o[2] = (uint8_t)p;
			o[3] = 255;
		}
	snprintf(name, sizeof name, "%s", game.title);
	for (i = 0; name[i]; i++)
		if (strchr("\\/:*?\"<>|", name[i]))
			name[i] = ' ';
	strftime(stamp, sizeof stamp, "%Y-%m-%d %H.%M.%S", localtime(&now));
	store_pictures_dir(dir, sizeof dir);
	snprintf(path, sizeof path, "%s/%s %s.png", dir, name, stamp);
	for (k = 2; !stat(path, &st) && k < 1000; k++)
		snprintf(path, sizeof path, "%s/%s %s %d.png", dir, name, stamp, k);
	if (png_write(path, rgba, v.w, h))
		snprintf(text, sizeof text, "The snapshot couldn't be saved");
	else
		snprintf(text, sizeof text, "Snapshot saved in %s", base_name(dir));
	notify(text);
	free(rgba);
}

/* The picture in view, blurred: averaged down to an eighth of its size, which the graphics chip
   then smooths back up to the picture's place, for behind the splash while a restored game loads */
static void make_blur(void)
{
	SDL_Rect v = view();
	int ys = emu_dbl ? 2 : 1, dh = v.h * ys, bw = v.w / 8 > 1 ? v.w / 8 : 1, bh = dh / 8 > 1 ? dh / 8 : 1, x, y, bx, by;
	uint32_t *px;

	if (blur_tex)
		SDL_DestroyTexture(blur_tex);
	blur_tex = NULL;
	if (emu_w <= 0 || v.w <= 0 || dh <= 0 || !(px = malloc((size_t)bw * bh * 4)))
		return;
	for (by = 0; by < bh; by++)
		for (bx = 0; bx < bw; bx++)
		{
			unsigned r = 0, g = 0, b = 0, n = 0;

			for (y = by * dh / bh; y < (by + 1) * dh / bh; y++)
				for (x = bx * v.w / bw; x < (bx + 1) * v.w / bw; x++)
				{
					uint32_t p = emu_fb[(size_t)(v.y + y / ys) * EMU_FB_W + v.x + x];

					r += p >> 16 & 255;
					g += p >> 8 & 255;
					b += p & 255;
					n++;
				}
			n = n ? n : 1;
			px[by * bw + bx] = (r / n) << 16 | (g / n) << 8 | b / n;
		}
	if ((blur_tex = SDL_CreateTexture(rg_ren, SDL_PIXELFORMAT_RGB888, SDL_TEXTUREACCESS_STATIC, bw, bh)))
	{
		SDL_UpdateTexture(blur_tex, NULL, px, bw * 4);
		SDL_SetTextureScaleMode(blur_tex, SDL_ScaleModeLinear);
		SDL_SetTextureBlendMode(blur_tex, SDL_BLENDMODE_BLEND);
	}
	free(px);
}

/* The blurred picture in the picture's place, at that much opacity out of 255, with the window
   darkened under the splash in proportion */
static void draw_blur(int alpha)
{
	SDL_Rect r;
	double zoom;
	int w, h;

	picture_rect(&r, &zoom);
	SDL_SetTextureAlphaMod(blur_tex, (Uint8)alpha);
	SDL_RenderCopy(rg_ren, blur_tex, NULL, &r);
	rg_size(&w, &h);
	rg_shade(0, 0, w, h, 0x000000, 100 * alpha / 255);
}

/* Starts writing the machine to a slot, with a screenshot. 0 once it has started, -1 if it can't just now. */
static int save_slot(int k)
{
	char path[1024], dir[1024];
	int sw = 0, sh = 0, r;
	uint32_t *shot = screenshot(&sw, &sh);

	store_game_path(game.id, "saves", dir, sizeof dir);
	make_dirs(dir);
	slot_path(k, path, sizeof path);
	r = state_save(path, 0, shot, sw, sh);
	free(shot);
	if (r)
		return -1;
	writing_slot = k;
	return 0;
}

/* Puts the machine back as a slot has it, and makes that the slot Auto Save writes */
static int restore_slot(int k)
{
	char path[1024], why[200], text[64];
	const char *in;
	int i;

	slot_path(k, path, sizeof path);
	if (state_load(path, why, sizeof why))
	{
		notify(why);
		return -1;
	}
	emu_script_stop();
	swap_pending = 0;
	in = machine_disc_path(0);
	for (i = 0; in && i < game.ndiscs; i++)
	{
		store_game_path(game.id, game.disc[i], path, sizeof path);
		if (!strcmp(path, in))
			cur_disc = i;
	}
	if (game.autosave != k + 1)
	{
		game.autosave = k + 1;
		store_save(&game);
	}
	armed = played = 1;
	paused = confirming = 0;
	snprintf(text, sizeof text, "Slot %d restored", k + 1);
	notify(text);
	return 0;
}

/* Says why nothing is being written, when Auto Save's slot holds a save from before this game started */
static void waiting_note(void)
{
	char text[80];

	if (armed || !game.autosave)
		return;
	snprintf(text, sizeof text, "Auto Save is waiting, as Slot %d holds an earlier save", game.autosave);
	notify(text);
}

/* ---- The selector ---- */

static void close_selector(void)
{
	int k;

	for (k = 0; k < SLOTS; k++)
		if (slot_tex[k])
		{
			SDL_DestroyTexture(slot_tex[k]);
			slot_tex[k] = NULL;
		}
	selector = erasing = 0;
}

/* Takes away a slot's save */
static void erase_save(int k)
{
	char path[1024];

	slot_path(k, path, sizeof path);
	remove(path);
	saved_at[k] = slot_time(k);
	if (slot_tex[k])
	{
		SDL_DestroyTexture(slot_tex[k]);
		slot_tex[k] = NULL;
	}
	if (game.autosave == k + 1)
		armed = 1;              /* nothing left there for Auto Save to keep */
}

/* Opens the selector with each slot's screenshot as its file has it now. The mouse is let go of,
   and nothing typed reaches the game while it is open. */
static void open_selector(void)
{
	char path[1024];
	int k;

	close_selector();
	for (k = 0; k < SLOTS; k++)
	{
		uint32_t *px;

		slot_path(k, path, sizeof path);
		if (!saved_at[k] || !(px = state_shot(path, &slot_w[k], &slot_h[k])))
			continue;
		if ((slot_tex[k] = SDL_CreateTexture(rg_ren, SDL_PIXELFORMAT_RGB888, SDL_TEXTUREACCESS_STATIC, slot_w[k], slot_h[k])))
		{
			SDL_UpdateTexture(slot_tex[k], NULL, px, slot_w[k] * 4);
			SDL_SetTextureScaleMode(slot_tex[k], SDL_ScaleModeLinear);
		}
		free(px);
	}
	selector = 1;
	set_capture(0);
	memset(emu_host_keys, 0, sizeof emu_host_keys);
}

/* A button in a red surround, laid out as the default button's cream one, for one that takes
   something away. The box is the surround's. */
static int red_button(int x, int y, int w, int h, const char *label)
{
	rg_bevel(x, y, w, h, 1);
	rg_fill(x + 2, y + 2, w - 4, h - 4, rg_colour(RG_RED));
	return rg_button(x + 4, y + 4, w - 8, h - 8, label, 1);
}

/* Asks whether to take a save away, to erase it or write over it, in a small box in the middle of
   the h high space below top, with Cancel and a red button that does it. That button takes a
   click: Return doesn't press it. Returns 1 to go ahead, 2 to keep the save, 0 for neither yet. */
static int ask(int w, int h, int top, const char *title, const char *text, const char *button)
{
	int tw, dw, dh, x, y, closed;

	tw = rg_text_w(RG_DESKTOP, text);
	dw = tw + 24 > 214 ? tw + 24 : 214;
	dh = 20 + 12 + 20 + 12 + 34 + 10;
	x = (w - dw) / 2;
	y = top + (h - dh) / 2;
	closed = rg_dialogue(x, y, dw, dh, title);
	rg_text_centre(RG_DESKTOP, x, y + 32, dw, 20, rg_colour(7), text);
	if (rg_button(x + dw - 12 - 92 - 12 - 84 + 4, y + dh - 10 - 30, 84, 26, "Cancel", 1) || closed || rg_in.key == SDLK_ESCAPE)
		return 2;
	return red_button(x + dw - 12 - 92, y + dh - 10 - 34, 92, 34, button);
}

/* The selector: a box with the three slots from left to right, each its screenshot and when it
   was saved, with Continue, which restores it, or for an empty slot New Game, and for a slot with
   a save Erase under that, which asks first. Closing the box goes on with the game as it is. In a
   small window, as at 1x, the box is laid out tighter. */
static void draw_selector(void)
{
	int w, h, top = fullscreen ? 0 : (int)(bar_px() / rg_scale + 0.5f), pw, th, dw, dh, x, y, k, closed, compact;
	int margin, pad, gap, bw, bh, name_h, time_h, under;
	int pressed = rg_in.pressed, released = rg_in.released, key = rg_in.key;

	rg_size(&w, &h);
	h -= top;
	compact = w < 480 || h < 360;
	margin = compact ? 4 : 16;      /* from the window's edge to the box */
	pad = compact ? 6 : 16;         /* inside the box, round the slots */
	gap = compact ? 4 : 12;         /* between the slots */
	bh = compact ? 22 : 26;         /* a button's height */
	name_h = compact ? 18 : 22;
	time_h = compact ? 14 : 18;
	under = 4 + name_h + time_h + 10 + bh + (compact ? 8 : 12) + bh;        /* the name, the time and the buttons, under the screenshot */
	pw = (w - 2 * margin - 2 * pad - 2 * gap) / 3;
	pw = pw > 240 ? 240 : pw;
	th = pw * 4 / 5;                /* a 640 by 512 screen's shape */
	if (th > h - 2 * margin - 20 - 2 * pad - under)
	{
		th = h - 2 * margin - 20 - 2 * pad - under;
		th = th < 24 ? 24 : th;
		pw = th * 5 / 4 > pw ? pw : th * 5 / 4;
	}
	bw = pw - 8 < 100 ? pw - 8 : 100;
	dw = 3 * pw + 2 * gap + 2 * pad;
	dh = 20 + 2 * pad + th + under;
	x = (w - dw) / 2;
	y = top + (h - dh) / 2;
	if (erasing || confirming)      /* the box asking about it takes the clicks and keys */
		rg_in.pressed = rg_in.released = rg_in.key = 0;
	closed = rg_dialogue(x, y, dw, dh, "Saves");
	for (k = 0; k < SLOTS; k++)
	{
		int px = x + pad + k * (pw + gap), py = y + 20 + pad, bx = px + (pw - bw) / 2, by = py + th + 4 + name_h + time_h + 10;
		char label[40], when[32], text[48];

		rg_slab(px, py, pw, th, slot_tex[k] ? 0x000000 : rg_colour(1), 1);
		if (slot_tex[k])
		{
			int iw = pw - 4, ih = th - 4;   /* the screenshot in its own shape, as large as fits */

			if (iw * slot_h[k] > ih * slot_w[k])
				iw = ih * slot_w[k] / slot_h[k];
			else
				ih = iw * slot_h[k] / slot_w[k];
			rg_image(slot_tex[k], px + (pw - iw) / 2, py + (th - ih) / 2, iw, ih);
		}
		else
			rg_text_centre(RG_DESKTOP, px, py, pw, th, rg_colour(4), saved_at[k] ? "No picture" : "Empty");
		snprintf(label, sizeof label, "Slot %d (Auto Save)", k + 1);
		if (game.autosave != k + 1 || rg_text_w(RG_DESKTOP_BOLD, label) > pw)
			snprintf(label, sizeof label, "Slot %d", k + 1);
		rg_text_centre(RG_DESKTOP_BOLD, px, py + th + 4, pw, name_h, rg_colour(7), label);
		if (saved_at[k])
		{
			saved_when(saved_at[k], when, sizeof when);
			snprintf(text, sizeof text, "Saved %s", when);
			rg_text_centre(SMALL, px, py + th + 4 + name_h, pw, time_h, rg_colour(6), rg_text_w(SMALL, text) > pw ? when : text);
		}
		if (rg_default_button(bx - 4, by - 4, bw + 8, bh + 8, saved_at[k] ? "Continue" : "New Game", 1))
		{
			if (saved_at[k])
				want_restore = k + 1;   /* which makes it the slot Auto Save writes */
			else
				want_new = k + 1;
			close_selector();
			return;
		}
		if (saved_at[k] && rg_button(bx, by + bh + (compact ? 8 : 12), bw, bh, "Erase", writing_slot != k))
			erasing = k + 1;
	}
	if (closed || rg_in.key == SDLK_ESCAPE)
	{
		close_selector();
		waiting_note();
		return;
	}
	rg_in.pressed = pressed;
	rg_in.released = released;
	rg_in.key = key;
	if (erasing)
	{
		char text[48];
		int r;

		if (rg_in.key == SDLK_RETURN)
			rg_in.key = 0;
		snprintf(text, sizeof text, "Erase the save in Slot %d?", erasing);
		r = ask(w, h, top, "Erase", text, "Erase");
		if (r == 1)
			erase_save(erasing - 1);
		if (r)
			erasing = 0;
	}
}

/* Asks whether Save is to write over a slot's save, the game stopped behind the box */
static void draw_confirm(void)
{
	int w, h, top = fullscreen ? 0 : (int)(bar_px() / rg_scale + 0.5f), r;
	char text[48];

	rg_size(&w, &h);
	if (rg_in.key == SDLK_RETURN)
		rg_in.key = 0;
	snprintf(text, sizeof text, "Replace the save in Slot %d?", confirming);
	r = ask(w, h - top, top, "Save", text, "Save");
	if (r == 1)
		want_save = confirming;
	if (r)
		confirming = 0;
}

/* A paused game's picture darkened, with Paused in a box in the middle of it */
static void draw_paused(void)
{
	int w, h, top = fullscreen ? 0 : (int)(bar_px() / rg_scale + 0.5f), bw = rg_text_w(RG_DESKTOP_BOLD, "Paused") + 48, bh = 40, x, y;

	rg_size(&w, &h);
	rg_shade(0, top, w, h - top, 0x000000, 40);
	x = (w - bw) / 2;
	y = top + (h - top - bh) / 2;
	rg_shade(x + 4, y + 4, bw, bh, 0x000000, 80);
	rg_fill(x, y, bw, bh, 0x000000);
	rg_fill(x + 1, y + 1, bw - 2, bh - 2, rg_colour(12));
	rg_text_centre(RG_DESKTOP_BOLD, x, y, bw, bh, 0x000000, "Paused");
}

/* Loads the shader the settings name, if any, in place of the one before */
static void start_shader(void)
{
	char path[1024], text[160];
	int i;

	shader_close();
	shader_on = 0;
	for (i = 0; can_shade && i < NSHADERS; i++)
		if (!strcmp(settings.shader, shaders[i].name))
		{
			if (!store_asset(shaders[i].preset, path, sizeof path))
				snprintf(text, sizeof text, "%s is missing from beside the program", shaders[i].name);
			else if (shader_open(path))
				snprintf(text, sizeof text, "%s couldn't start on this computer's graphics", shaders[i].name);
			else
				shader_on = 1;
			if (!shader_on)
				notify(text);
		}
}

/* What the menus hold just now, in order. Returns how many items. */
static int menu_items(menu_item_t *it, int booting)
{
	int n = 0, i, slot, sub;
	char when[32];

	memset(it, 0, sizeof(menu_item_t) * MAX_ITEMS);
#define ITEM(m, c, tick, line, ...) \
	(it[n].menu = m, it[n].cmd = c, it[n].checked = tick, it[n].rule = line, snprintf(it[n].label, sizeof it[n].label, __VA_ARGS__), n++)
	ITEM(0, CMD_PAUSE, paused, 0, "Pause");
	it[n - 1].shaded = booting;
	ITEM(0, CMD_SEND_ESC, 0, 0, "Send ESC");
	ITEM(0, CMD_FULLSCREEN, fullscreen, 1, "Fullscreen");
	for (i = 1; i <= 3; i++)
	{
		ITEM(0, CMD_SCALE + i, !fullscreen && settings.scale == i && !own_size(), 0, "%dx Scale", i);
		it[n - 1].shaded = i == 1 && shader_on;         /* a shader draws at 2x */
	}
	ITEM(0, 0, 0, 0, "Shaders");
	sub = n;
	ITEM(0, CMD_SHADER, !settings.shader[0], 0, "Off");
	it[n - 1].sub = sub;
	for (i = 0; i < NSHADERS; i++)
	{
		ITEM(0, CMD_SHADER + 1 + i, !strcmp(settings.shader, shaders[i].name), !i, "%s", shaders[i].name);
		it[n - 1].sub = sub;
		it[n - 1].shaded = !can_shade;
	}
	ITEM(0, CMD_CROP, crop, 1, "Crop to Window");
	ITEM(0, CMD_SNAPSHOT, 0, 1, "Save Snapshot");
	it[n - 1].shaded = booting;
	ITEM(0, CMD_CLOSE, 0, 1, "Close");
	for (i = 0; i < SLOTS; i++)     /* each slot a submenu of its own */
	{
		ITEM(1, 0, game.autosave == i + 1, 0, "Slot %d", i + 1);
		slot = n;
		ITEM(1, CMD_AUTO + i, game.autosave == i + 1, 0, "Selected for Auto Save");
		it[n - 1].sub = slot;
		saved_when(saved_at[i], when, sizeof when);
		if (saved_at[i])
			ITEM(1, CMD_RESTORE + i, 0, 0, "Restore (Last saved %s)", when);
		else
			ITEM(1, CMD_RESTORE + i, 0, 0, "Restore (Empty)");
		it[n - 1].sub = slot;
		it[n - 1].shaded = !saved_at[i];
		ITEM(1, CMD_SAVE + i, 0, 0, "Save");
		it[n - 1].sub = slot;
		it[n - 1].shaded = booting;
	}
	ITEM(1, CMD_SELECTOR, selector, 1, "Open Selector");
	for (i = 0; i < game.ndiscs; i++)
		ITEM(2, CMD_DISC + i, i == cur_disc, 0, "Disk %d", disk_number(i));
	ITEM(2, CMD_SET_SWAP, swap_pending, 1, "Set Auto Switch Point");
	if (n_swaps)
		ITEM(2, CMD_CLEAR_SWAPS, 0, 0, "Clear Auto Switch Points");
#undef ITEM
	return n;
}

/* Does what a menu item says. Returns 1 if the game is to close. */
static int menu_command(int cmd)
{
	if (cmd >= CMD_AUTO && cmd < CMD_AUTO + SLOTS)          /* the slot Auto Save writes, or none */
	{
		game.autosave = game.autosave == cmd - CMD_AUTO + 1 ? 0 : cmd - CMD_AUTO + 1;
		armed = 1;
		store_save(&game);
	}
	else if (cmd >= CMD_RESTORE && cmd < CMD_RESTORE + SLOTS && saved_at[cmd - CMD_RESTORE])
	{
		want_restore = cmd - CMD_RESTORE + 1;
		close_selector();               /* as its Continue would */
	}
	else if (cmd >= CMD_SAVE && cmd < CMD_SAVE + SLOTS && saved_at[cmd - CMD_SAVE] && cmd - CMD_SAVE + 1 != game.autosave)
	{
		confirming = cmd - CMD_SAVE + 1;        /* a save that isn't Auto Save's is written over only once the player says */
		erasing = 0;
		set_capture(0);
		memset(emu_host_keys, 0, sizeof emu_host_keys);
	}
	else if (cmd >= CMD_SAVE && cmd < CMD_SAVE + SLOTS)
		want_save = cmd - CMD_SAVE + 1;
	else if (cmd == CMD_PAUSE)
	{
		paused = !paused;
		if (paused)
		{
			set_capture(0);
			memset(emu_host_keys, 0, sizeof emu_host_keys);
		}
		else
			emu_audio_fade_in(300);
	}
	else if (cmd == CMD_SNAPSHOT)
		save_snapshot();
	else if (cmd == CMD_SELECTOR)
	{
		if (selector)
			close_selector();
		else
			open_selector();
	}
	else if (cmd == CMD_FULLSCREEN)
		set_fullscreen(!fullscreen);
	else if (cmd >= CMD_SHADER && cmd <= CMD_SHADER + NSHADERS)
	{
		int fitted = !fullscreen && !own_size();

		snprintf(settings.shader, sizeof settings.shader, "%s", cmd > CMD_SHADER ? shaders[cmd - CMD_SHADER - 1].name : "");
		start_shader();
		if (shader_on && settings.scale < 2)    /* a window at 1x Scale goes to 2x, the shader's size */
		{
			settings.scale = 2;
			if (fitted)
				fit_window();
		}
		store_save_settings(&settings);
	}
	else if (cmd > CMD_SCALE && cmd <= CMD_SCALE + 3)
	{
		settings.scale = cmd - CMD_SCALE;
		store_save_settings(&settings);
		game.win_w = game.win_h = 0;    /* the window opens at this size next time, not one it was dragged to */
		set_fullscreen(0);
		fit_window();
	}
	else if (cmd == CMD_CLOSE)
		return 1;
	else if (cmd == CMD_SET_SWAP)
	{
		if (swap_pending)
			swap_pending = 0;
		else if (n_swaps >= MAX_SWAPS)
			notify("This game has all the auto switch points it can hold.");
		else
		{
			signature(pending_sig);
			swap_pending = 1;
			notify("Now pick the disk to switch to when the game shows this screen.");
		}
	}
	else if (cmd == CMD_CLEAR_SWAPS)
	{
		n_swaps = swap_pending = 0;
		swaps_save();
		notify("This game's auto switch points are cleared.");
	}
	else if (cmd == CMD_CROP)               /* just the game's windows, or the whole screen, kept for next time */
	{
		game.crop = crop = !crop;
		crop_fitted = 0;
		set_capture(0);
		if (!fullscreen && !crop)
			fit_window();
		store_save(&game);
	}
	else if (cmd == CMD_SEND_ESC)           /* Escape on the keyboard frees the mouse, so the machine gets it from here */
		emu_script("key esc down\nwait 5\nkey esc up\n");
	else if (cmd >= CMD_DISC && cmd < CMD_DISC + game.ndiscs)
		choose_disc(cmd - CMD_DISC);
	return 0;
}

/* Ctrl+Alt+S and Ctrl+Alt+R save to and restore the slot selected for Auto Save, as its Save and
   Restore in the Saves menu do, and Ctrl+Alt+P pauses */
static void quick_key(int key)
{
	char text[64];
	int k = game.autosave - 1;

	if (key == SDLK_p)
		menu_command(CMD_PAUSE);
	else if (k < 0)
		notify("No slot is selected for Auto Save");
	else if (key == SDLK_s)
		menu_command(CMD_SAVE + k);
	else if (saved_at[k])
		menu_command(CMD_RESTORE + k);
	else
	{
		snprintf(text, sizeof text, "Slot %d is empty", k + 1);
		notify(text);
	}
}

/* A notice in the bottom right corner: a cream box with a black edge */
static void draw_note(void)
{
	int w, h, tw;

	if (!note[0] || SDL_GetTicks() > note_until)
		return;
	rg_size(&w, &h);
	tw = rg_text_w(RG_DESKTOP, note);
	rg_shade(w - tw - 34, h - 42, tw + 26, 32, 0x000000, 80);
	rg_fill(w - tw - 38, h - 46, tw + 26, 32, 0x000000);
	rg_fill(w - tw - 37, h - 45, tw + 24, 30, rg_colour(12));
	rg_text(RG_DESKTOP, w - tw - 25, h - 46 + (32 - rg_font_height(RG_DESKTOP)) / 2, 0x000000, note);
}

/* What the window shows while the game boots behind it: the game's icon and name on the marbled
   grey, and a bar that fills as the boot goes */
static void draw_splash(int done, int total)
{
	int w, h;

	rg_size(&w, &h);
	if (!blur_tex)
		rg_marble(0, 0, w, h);
	rg_image(icon_tex, w / 2 - 64, h / 2 - 110, 128, 128);
	rg_text_centre(RG_DESKTOP_BOLD, 0, h / 2 + 30, w, 20, blur_tex ? 0xffffff : 0x000000, game.title);
	rg_slab(w / 2 - 110, h / 2 + 62, 220, 16, rg_colour(0), 1);
	rg_fill(w / 2 - 108, h / 2 + 64, total > 0 ? 216 * (done > total ? total : done) / total : 0, 12, rg_colour(8));
}

/* The keys and steps that start the game, and how long they take in centiseconds. A disc that
   boots gets Shift held until the disc is first read, a little over a second in, and no longer: some games
   set the machine up and reset it, and Shift held at a reset tells RISC OS not to boot. Any
   other disc has its root opened and its application run by HostLink once the desktop is up, as
   a double-click would. */
/* A game that starts from an application on its disc is started by ROO311's HostLink module, as
   a double-click would, with no command line opened over the desktop: a Classics game as soon as
   the desktop is up, a JASPP game once it has settled (its banner is gone 9.3 seconds in). From
   there HostLink says what the game does. It changes screen mode when it takes the screen, opens
   an error box when it asks the person something (a disc to put in, say), or starts as a task in
   the desktop, and then its icon on the icon bar is clicked for it, up to 3 times 2 seconds apart
   for an icon that comes a little later. A game that sets the video controller up itself, without
   telling RISC OS, has taken the screen when the controller is set up. */
static struct
{
	int run_at, started, ready, clicked, missed, tries, click_at;
	int mode;               /* the screen mode its presets give the desktop, for a game that plays in it, or 0 */
	int task_at;            /* when it became a task in the desktop, or 0 */
	int drive_at;           /* when the floppy drive last turned */
	int keyed;              /* the keys its presets name have been pressed */
	int run_due, run_from, run_reads, run_quiet;    /* its folder opened, the application still to run, and the drive's reads since */
} app = { -1 };

/* Where the game's application is: the floppy in drive 0, or the HostFS folder of a game with a
   support pack */
static const char *app_root(void)
{
	return emu_hostfs_dir ? "HostFS:$" : "adfs::0.$";
}

static void close_root(void)
{
	char text[64];

	snprintf(text, sizeof text, "Filer_CloseDir %s", app_root());
	emu_host_run(text);
}

static void start_app(void)
{
	char text[512];

	static const char *const pack[] = { "!Boot", "!System", "!Scrap" };
	size_t i;

	app.started = 1;
	app.clicked = emu_host_clicked;
	app.missed = emu_host_missed;
	for (i = 0; emu_hostfs_dir && i < sizeof pack / sizeof *pack; i++)
	{
		/* the support pack's applications first, as the Filer boots them when it sees them */
		snprintf(text, sizeof text, "%s/%s", emu_hostfs_dir, pack[i]);
		if (!is_dir(text))
			continue;
		snprintf(text, sizeof text, "Filer_Boot HostFS:$.%s", pack[i]);
		emu_host_run(text);
	}
	snprintf(text, sizeof text, "Filer_OpenDir %s", app_root());
	emu_host_run(text);
	app.run_due = 1;                /* app_events runs it once the Filer has read the folder */
	app.run_from = app.run_quiet = now_cs;
	app.run_reads = machine_disc_reads;
}

/* The keys a game's presets name (keys=, by the names test scripts use), pressed behind the splash
   for a choice the game doesn't keep from one time to the next: once the game is going, or for
   one that plays in the desktop, once it has loaded and before its icon is clicked, to answer a
   question that comes first. They wait a second first, as a box that has just opened doesn't
   take keys yet. */
static int press_keys(void)
{
	char keys[120], text[600] = "wait 100\n";
	char *k;
	size_t used = strlen(text);

	if (app.keyed++ || !preset_setting(game.preset[0] ? game.preset : game.id, "keys", keys, sizeof keys))
		return 0;
	for (k = strtok(keys, " "); k && used + 80 < sizeof text; k = strtok(NULL, " "))
		used += (size_t)snprintf(text + used, sizeof text - used, "key %s down\nwait 5\nkey %s up\nwait 10\n", k, k);
	snprintf(text + used, sizeof text - used, "wait 50\n");
	emu_script(text);
	return 1;
}

/* The game is going */
static void app_ready(int close)
{
	app.ready = 1;
	if (close)
		close_root();
	press_keys();
}

/* Acts on what HostLink has said since the game was started */
static void app_events(int cs)
{
	now_cs = cs;
	if (app.ready && !crop_from)
		crop_from = cs + 100;
	if (app.run_at >= 0 && cs >= app.run_at)
	{
		app.run_at = -1;
		start_app();
	}
	/* The application runs a second after its folder was opened, once the drive has been quiet
	   for half a second: run while the Filer still reads the folder, FileSwitch can be left part
	   way through, and Fire and Ice then stops with an address exception inside it */
	if (app.run_due)
	{
		if (machine_disc_reads != app.run_reads)
		{
			app.run_reads = machine_disc_reads;
			app.run_quiet = cs;
		}
		if (cs >= app.run_from + 100 && cs >= app.run_quiet + 50)
		{
			char text[512];

			app.run_due = 0;
			snprintf(text, sizeof text, "Filer_Run %s.%s", app_root(), game.boot);
			emu_host_run(text);
		}
		return;
	}
	if (!app.started || app.ready)
		return;
	/* since HostLink started it, it has taken the screen or asked the person something. A game
	   whose presets give the desktop a mode plays in the desktop, and setting that mode again for
	   its loading screen isn't taking the screen. */
	if (emu_host_modes_at_run >= 0 && (emu_host_errors > emu_host_errors_at_run ||
					   (!app.mode && (emu_host_modes > emu_host_modes_at_run || machine_mode_sets > emu_host_vidc_at_run))))
	{
		emu_host_forget();              /* nothing left over to send when it ends */
		app_ready(0);
		return;
	}
	if (emu_host_tasks_at_run < 0 || emu_host_tasks <= emu_host_tasks_at_run)   /* not yet a task in the desktop */
		return;
	if (!app.tries)         /* a game whose presets say click=0 waits for something other than a click on its icon */
	{
		char click[8];

		if (preset_setting(game.preset[0] ? game.preset : game.id, "click", click, sizeof click) && !atoi(click))
		{
			app_ready(0);
			return;
		}
	}
	if (emu_host_clicked > app.clicked)
	{
		if (game.classic || game.apps || app.mode)      /* a desktop game, going; a JASPP game takes the screen or asks for a disc next */
			app_ready(1);
		return;
	}
	/* A game that plays in the desktop in its own mode may put its icon up before it looks for a
	   click on it, and go on loading from its disc, so it is clicked once it has had 2 seconds and
	   the drive has been still for 1, or after 30 seconds whatever it is doing */
	if (!app.task_at)
		app.task_at = cs;
	if (machine_motor_on())
		app.drive_at = cs;
	if (app.tries == 0 && app.mode && cs < app.task_at + 3000 && (cs < app.task_at + 200 || cs < app.drive_at + 100))
		return;
	if (app.tries == 0 && app.mode && !app.keyed && press_keys())
		app.click_at = cs + 200;        /* and its icon clicked once they have had time to answer */
	if (app.tries == 0 && app.mode && (cs < app.click_at || emu_script_busy()))
		return;
	if (app.tries == 0 || (emu_host_missed > app.missed && cs >= app.click_at))
	{
		if (app.tries == 3)             /* no icon to click: it plays in a window of its own */
		{
			app_ready(1);
			return;
		}
		app.missed = emu_host_missed;
		app.tries++;
		app.click_at = cs + 200;
		emu_host_click(game.boot);
	}
}

static int boot_script(void)
{
	if (!game.boot[0])
	{
		emu_script("key shift down\nuntilread 600\nkey shift up\n");
		return 270;
	}
	app.run_at = game.classic || game.apps ? 0 : 1000;
	return game.classic || game.apps ? 1500 : 1300;
}

/* Holds the game for this copy of PiArcBoot, so that no other copy plays it and writes its saves
   at the same time: a file in its folder kept open with a lock, which the system lets go of when
   the program ends. 0 if another copy holds it. */
static int hold_game(void)
{
	char path[1024];
	int fd;

	store_game_path(game.id, "game.lock", path, sizeof path);
#ifdef _WIN32
	fd = _sopen(path, _O_RDWR | _O_CREAT | _O_NOINHERIT, _SH_DENYRW, _S_IREAD | _S_IWRITE);
	return fd >= 0 || errno != EACCES;
#else
	fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
	return fd < 0 || !flock(fd, LOCK_EX | LOCK_NB) || errno != EWOULDBLOCK;
#endif
}

/* For a game another copy has open: a window of its own with a box that says so, until OK */
static void say_open(const char *cls, const uint8_t *icon, const char *test_script)
{
	char text[200];
	int w, h, done = 0, frame = 0;

	if (rg_open(game.title, RG_ERROR_W + 48, RG_ERROR_H + 20 + 48, 0, cls, icon))
		return;
	if (test_script)
		rg_test_load(test_script);
	snprintf(text, sizeof text, "%s is already open in another window", game.title);
	while (!done)
	{
		const char *shot = NULL, *drop = NULL;
		SDL_Event e;

		rg_begin();
		while (SDL_PollEvent(&e))
		{
			rg_event(&e);
			done |= e.type == SDL_QUIT;
		}
		if (test_script)
			done |= rg_test_step(frame++, &shot, &drop);
		rg_size(&w, &h);
		rg_marble(0, 0, w, h);
		done |= rg_error_box_with((w - RG_ERROR_W) / 2, (h - RG_ERROR_H + 20) / 2, "Message from PiArcBoot", text, RG_INFORMATION, "OK", NULL) ||
			rg_in.key == SDLK_ESCAPE;
		if (shot)
			rg_save_shot(shot);
		rg_present();
		SDL_Delay(10);
	}
	SDL_Quit();
}

int game_main(const char *id, const char *test_script)
{
	char path[1024], cls[160], rom[1024], extrom[1024], hostfs[1024];
	uint8_t icon[128 * 128 * 4], cmos[256];
	int booting = 1, script_cs, cs = 0, script_end = -1, settle = -1, modes_at_read = 0, last_check = 0, w, h, quit = 0, i, auto_at = 0;
	int drive_on = 0;       /* when the floppy drive's motor was last seen turning, from the first read on */
	int changed = 0, sig_at = 0;    /* the picture differs from the one there at the first read, as last looked at then */
	uint8_t sig_read[GRID];
	int held = 0;           /* frames a test has waited with the machine stopped, on the selector or paused, which its script's times count too */
	Uint32 splash_until = 0;        /* after a slot is picked, the splash stays up till then, SDL_GetTicks's time, and the game waits behind it */
	int run_for = getenv("PIARCBOOT_RUN_FOR") ? atoi(getenv("PIARCBOOT_RUN_FOR")) : 0, lost = 0, shown_cs = 0, drawn = 0;
	menu_item_t items[MAX_ITEMS], bar_items[MAX_ITEMS];
	int n_items;
	Uint64 freq, start, began, shown = 0;
	Uint64 frame_ticks = 0, next_frame;     /* a refresh in performance counter ticks, when the game keeps its own pace, and when its next frame is due */
	size_t n;

	deploy_installed(id);           /* a game its installer put on HostFS starts from a fresh copy if its own is gone */
	store_game_path(id, "hostfs", hostfs, sizeof hostfs);
	if (store_load(id, &game) || (!game.ndiscs && !is_dir(hostfs)))         /* discs, or a HostFS folder */
	{
		fprintf(stderr, "piarcboot: no game called %s is installed\n", id);
		return 1;
	}
	if (!store_asset("ROM311", rom, sizeof rom))
	{
		fprintf(stderr, "piarcboot: the ROM image ROM311 is missing from beside the program\n");
		return 1;
	}
	store_icon(&game, icon);
	snprintf(cls, sizeof cls, "piarcboot-%s", game.id);
	for (n = 0; cls[n]; n++)
		if (!((cls[n] | 32) >= 'a' && (cls[n] | 32) <= 'z') && !(cls[n] >= '0' && cls[n] <= '9') && cls[n] != '-' && cls[n] != '_')
			cls[n] = '_';
	if (!hold_game())
	{
		fprintf(stderr, "piarcboot: %s is already open in another window\n", game.title);
		say_open(cls, icon, test_script);
		return 1;
	}
	store_settings(&settings);
	if (settings.shader[0] && settings.scale < 2)
		settings.scale = 2;
	/* The renderer works in OpenGL, which the shaders run in: OpenGL ES on a Raspberry Pi, whose
	   OpenGL is too old for them. SDL_RENDER_DRIVER still has the last word. */
#if defined(__arm__) || defined(__aarch64__)
	SDL_SetHint(SDL_HINT_RENDER_DRIVER, "opengles2");
#else
	SDL_SetHint(SDL_HINT_RENDER_DRIVER, "opengl");
#endif
	if (rg_open(game.title, 320 * settings.scale, 256 * settings.scale, RG_RESIZABLE, cls, icon))
	{
		fprintf(stderr, "piarcboot: %s\n", SDL_GetError());
		return 1;
	}
	can_shade = shader_possible();
	start_shader();
	SDL_InitSubSystem(SDL_INIT_AUDIO);      /* a machine with no sound still plays, silently */
	/* The menu bar is the system's own where it has one to give */
	n_menus = game.ndiscs > 1 ? MENUS : MENUS - 1;
	for (i = 0; i < SLOTS; i++)
		saved_at[i] = slot_time(i);
	armed = !game.autosave || !saved_at[game.autosave - 1];         /* a save from before isn't written over until the player says */
	if (getenv("PIARCBOOT_RESTORE"))        /* PIARCBOOT_RESTORE=<slot>: restored as soon as it can be, for the tests */
		want_restore = atoi(getenv("PIARCBOOT_RESTORE")) >= 1 && atoi(getenv("PIARCBOOT_RESTORE")) <= SLOTS ? atoi(getenv("PIARCBOOT_RESTORE")) : 0;
	for (i = 0; !want_restore && i < SLOTS; i++)    /* a game with saves opens on the selector, and boots once one is picked */
		if (saved_at[i])
		{
			open_selector();
			break;
		}
	native_bar = menubar_open(menu_titles, n_menus);
	n_items = menu_items(bar_items, 1);
	if (native_bar)
		menubar_set(bar_items, n_items);
	SDL_SetWindowMinimumSize(rg_win, rg_to_window(320 * pic_scale()), rg_to_window(200 * pic_scale() + bar_px()));
	fit_window();
	if (game.win_w > 0 && game.win_h > 0)  /* the size the player last dragged it to */
		SDL_SetWindowSize(rg_win, game.win_w, game.win_h);
	icon_tex = rg_image_texture(icon, 128, 128, 1);
	if (game.fullscreen)
		set_fullscreen(1);

	machine_fixed_time = test_script != NULL;
	memcpy(cmos, default_cmos, sizeof cmos);
	if (game.classic || game.apps)  /* a 640 by 512 desktop in 256 colours, mode 21 on a multisync monitor, for the games that play in it */
		machine_cmos_desktop(cmos, 21, 1);
	crop = game.crop >= 0 ? game.crop : preset_setting(game.preset[0] ? game.preset : game.id, "crop", path, sizeof path) && atoi(path);
	/* The desktop in the screen mode a game's presets name, for one that asks for a mode, and
	   otherwise for a game shown cropped to its windows, which plays in the desktop, the Classics'
	   640 by 512 in 256 colours */
	if (preset_setting(game.preset[0] ? game.preset : game.id, "mode", path, sizeof path))
		app.mode = atoi(path);
	else if (crop)
		app.mode = 21;
	if (app.mode > 0)
		machine_cmos_desktop(cmos, app.mode, 1);
	/* A game with a support pack has modules in an extension ROM, which RISC OS takes up at power on
	   in place of older ones of the same name, and a folder of the computer's own as HostFS */
	store_game_path(game.id, "extrom", extrom, sizeof extrom);
	emu_extrom_dir = is_dir(extrom) ? extrom : NULL;
	emu_a3000 = game.a3000;
	machine_real_drive = game.real_drive;
	emu_hostfs_dir = is_dir(hostfs) ? hostfs : NULL;
	if (emu_start(rom, cmos, game.fdc))
	{
		fprintf(stderr, "piarcboot: the ROM image %s can't be read\n", rom);
		return 1;
	}
	if (!test_script)
		emu_audio_open();
	else
		rg_test_load(test_script);
	if (game.ndiscs)
	{
		store_game_path(game.id, game.disc[0], path, sizeof path);
		machine_disc(0, path, game.ro[0]);
	}
	swaps_load();
	power_on = state_take(&power_on_len);
	script_cs = boot_script();
	emu_mute = 1;

	freq = SDL_GetPerformanceFrequency();
	start = began = next_frame = SDL_GetPerformanceCounter();
	/* On a Wayland desktop the compositor puts each frame on the screen whole, at its own refresh,
	   so the game needn't wait for the refresh itself. Waiting on the compositor is what holds a
	   Pi 400 to 40 frames a second in full screen. The game keeps the display's pace instead. */
	if (getenv("WAYLAND_DISPLAY") && !SDL_RenderSetVSync(rg_ren, 0))
	{
		SDL_DisplayMode mode;
		int hz = !SDL_GetCurrentDisplayMode(SDL_GetWindowDisplayIndex(rg_win), &mode) && mode.refresh_rate > 0 ? mode.refresh_rate : 60;

		frame_ticks = freq / hz;
	}
	while (!quit)
	{
		Uint64 frame_start = SDL_GetPerformanceCounter();
		const char *shot = NULL, *drop = NULL;
		SDL_Event e;

		rg_begin();
		while (SDL_PollEvent(&e))
		{
			rg_event(&e);
			if (native_bar)
				menubar_event(&e);
			switch (e.type)
			{
				case SDL_QUIT:
				quit = 1;
				break;
				case SDL_KEYDOWN:
				case SDL_KEYUP:
				{
					int sc = e.key.keysym.scancode, down = e.type == SDL_KEYDOWN, key = e.key.keysym.sym;
					int mod = SDL_GetModState(), ctrl_alt = (mod & KMOD_CTRL) && (mod & KMOD_ALT), disk = -1;

					/* PiArcBoot's own keys are Ctrl+Alt ones, which Archimedes games leave alone, so Alt
					   and the rest are all the game's */
					for (i = 0; ctrl_alt && sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9 && i < game.ndiscs; i++)
						if (disk_number(i) == sc - SDL_SCANCODE_1 + 1)
							disk = i;
					if (ctrl_alt && sc == SDL_SCANCODE_RETURN)
					{
						if (down && !e.key.repeat)
							set_fullscreen(!fullscreen);
					}
					else if (ctrl_alt && (key == SDLK_s || key == SDLK_r || key == SDLK_p))
					{
						if (down && !e.key.repeat && (!booting || key == SDLK_r))      /* a restore can end the boot, as the menu's does */
							quick_key(key);
					}
					else if (sc == SDL_SCANCODE_ESCAPE)    /* frees the mouse, and never reaches the machine */
					{
						if (down)
							set_capture(0);
					}
					else if (disk >= 0)                     /* Ctrl+Alt and a number: that disk, if the game has it, though not while it boots */
					{
						if (down && !e.key.repeat && !booting)
							choose_disc(disk);
					}
					else if (sc < 512 && !booting && !stopped())
						emu_host_keys[sc] = down;
					break;
				}
				case SDL_MOUSEMOTION:
				if (captured)
				{
					SDL_Rect r;
					double zoom;

					picture_rect(&r, &zoom);        /* the pointer moves as far on screen as the mouse does */
					mouse_fx += e.motion.xrel / zoom;
					mouse_fy += e.motion.yrel / zoom;
				}
				break;
				case SDL_WINDOWEVENT:
				if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST)
				{
					set_capture(0);
					memset(emu_host_keys, 0, sizeof emu_host_keys);
				}
				/* A size the player drags the window to is kept for next time, and one from the
				   menu is not */
				if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED && !fullscreen && !(SDL_GetWindowFlags(rg_win) & SDL_WINDOW_MAXIMIZED))
				{
					SDL_GetWindowSize(rg_win, &w, &h);
					game.win_w = own_size() ? w : 0;
					game.win_h = own_size() ? h : 0;
				}
				break;
			}
		}
		if (test_script)
			quit |= rg_test_step(cs + held, &shot, &drop);

		/* A click on the picture gives the game the mouse. A game set not to capture it, or shown
		   cropped to its windows, has the RISC OS pointer put where the host's is over the picture,
		   by ROO311's HostLink, and the buttons pressed there. */
		if ((game.nocapture || crop) && emu_hostlink && !stopped())
		{
			SDL_Rect r, v = view();
			double zoom;
			int px, py, over;
			Uint32 b;

			picture_rect(&r, &zoom);
			px = v.x + (int)((rg_in.x * rg_scale - r.x) / zoom);
			py = v.y + (int)((rg_in.y * rg_scale - r.y) / zoom) / (emu_dbl ? 2 : 1);
			over = !booting && rg_in.x >= 0 && px >= v.x && px < v.x + v.w && py >= v.y && py < v.y + v.h;
			if (over)       /* in OS units, as many as the mode's eigen factors make a pixel */
			{
				int x2, y2;

				machine_os_per_pixel(&x2, &y2);
				emu_host_pointer(px * x2 / 2, (emu_h - 1 - py) * (emu_dbl ? 2 : 1) * y2 / 2);
			}
			b = over ? SDL_GetMouseState(NULL, NULL) : 0;
			emu_mouse(0, 0, (b & SDL_BUTTON_LMASK ? 1 : 0) | (b & SDL_BUTTON_RMASK ? 2 : 0) | (b & SDL_BUTTON_MMASK ? 4 : 0));
		}
		else if (!booting && !stopped() && rg_in.pressed && !captured && (fullscreen || rg_in.y * rg_scale >= bar_px()))
			set_capture(1);
		if (captured)
		{
			Uint32 b = SDL_GetMouseState(NULL, NULL);
			int dx = (int)mouse_fx, dy = (int)mouse_fy;

			mouse_fx -= dx;
			mouse_fy -= dy;
			emu_mouse(dx, dy, (b & SDL_BUTTON_LMASK ? 1 : 0) | (b & SDL_BUTTON_RMASK ? 2 : 0) | (b & SDL_BUTTON_MMASK ? 4 : 0));
		}
		else if (!((game.nocapture || crop) && emu_hostlink && !stopped()))
			emu_mouse(0, 0, 0);

		/* Run the machine: flat out behind the splash and in the tests, in real time otherwise */
		if (booting || test_script)
		{
			int wait = stopped() || (!booting && emu_mute);       /* nothing runs until a slot is picked, nor till the splash is down, nor while it is stopped */
			int limit = wait ? 0 : test_script && !booting ? 5 : 100000;

			held += wait && test_script;

			for (i = 0; i < limit; i++)
			{
				played |= !booting;
				emu_step();
				cs++;
				app_events(cs);
				/* The splash comes down once the keys have all been pressed and the game has
				   either put up a screen of its own since it began to load, or stopped loading,
				   and for a game cropped to its windows once the cropping has started */
				if (!modes_at_read && machine_disc_reads)
				{
					modes_at_read = machine_mode_sets + 1;
					drive_on = cs;
					signature(sig_read);
				}
				if (machine_motor_on())
					drive_on = cs;
				/* A disc that boots by itself has put up a screen of its own once the picture is no longer
				   what it was when it began to load, which shows a loading screen as soon as it is drawn.
				   RISC OS's start-up text, or the screen cleared, isn't one: Starch's loader sets the video
				   controller up and loads for most of a minute with the start-up text still there. */
				if (!game.boot[0] && modes_at_read && !changed && cs - sig_at >= 10)
				{
					uint8_t now[GRID];

					sig_at = cs;
					signature(now);
					changed = sig_diff(now, sig_read) > GRID / 25;
				}
				if (booting && !emu_script_busy())
				{
					if (script_end < 0)
						script_end = cs;
					if (settle < 0 && (game.boot[0] ? app.ready && !emu_script_busy() && (!crop || (crop_from && cs >= crop_from)) :
							   modes_at_read && changed))
						settle = cs + 15;
					if ((settle >= 0 && cs >= settle) || cs > script_end + (game.boot[0] ? (app.mode ? 9000 : 6000) : cs < drive_on + 100 ? 9000 : 3000))
					{
						if (test_script)
							fprintf(stderr, "picture shown at %d cs%s\n", cs, settle < 0 ? " (it never set a mode or stopped loading)" : "");
						booting = 0;
						break;
					}
				}
				if (rg_test_next() >= 0 && rg_test_next() <= cs + held)
					break;
				if ((SDL_GetPerformanceCounter() - frame_start) * 1000 / freq >= 14)
					break;
			}
		}
		else
		{
			int due;

			if (stopped())          /* its time stands still, and it goes on from there */
				start = SDL_GetPerformanceCounter() - (Uint64)cs * freq / 100;
			due = (int)((SDL_GetPerformanceCounter() - start) * 100 / freq) - cs;
			if (due > 10)           /* too far behind to catch up: let the time go */
			{
				start += (Uint64)(due - 10) * freq / 100;
				lost += due - 10;
				due = 10;
			}
			if (run_for && (SDL_GetPerformanceCounter() - shown) / freq >= (Uint64)run_for)
				quit = 1;
			for (i = 0; !stopped() && !emu_mute && i < due; i++)   /* the game stops while the selector or the splash is up, or it is paused */
			{
				played = 1;
				emu_step();
				cs++;
				app_events(cs);
			}
			if (due <= 0)
				SDL_Delay(1);
		}

		/* A slot to restore or save to, once the floppy drive has finished what it is doing. A slot
		   restored while the game boots ends the boot there. Either way the game comes up from behind
		   the splash, after 2 seconds of it, so a pick doesn't land in the middle of play. */
		if (want_restore && state_ready())
		{
			if (!restore_slot(want_restore - 1))
			{
				if (booting)
				{
					booting = 0;
					app.run_at = -1;
					app.started = app.ready = 1;
					crop_from = cs;
					emu_host_forget();
				}
				emu_mute = 1;
				for (i = 0; i < 8; i++)        /* unheard, till the restored picture has been drawn */
				{
					emu_step();
					cs++;
					app_events(cs);
				}
				if (!fullscreen && !own_size())
					fit_window();
				make_blur();
				splash_until = SDL_GetTicks() + 2000;
			}
			else if (booting)
				open_selector();        /* nothing started yet: another slot can be picked */
			want_restore = 0;
		}
		/* New Game in an empty slot: Auto Save writes that slot from here, and a game that has
		   been played starts again from power on */
		if (want_new && (!played || state_ready()))
		{
			char text[64];
			int k = want_new - 1;

			want_new = paused = 0;
			game.autosave = k + 1;
			store_save(&game);
			armed = 1;
			if (played && power_on && !state_put(power_on, power_on_len))
			{
				int mode = app.mode;

				emu_script_stop();
				memset(&app, 0, sizeof app);
				app.run_at = -1;
				app.mode = mode;
				emu_host_modes_at_run = emu_host_errors_at_run = emu_host_tasks_at_run = emu_host_vidc_at_run = -1;
				machine_disc_reads = machine_mode_sets = 0;
				booting = emu_mute = 1;
				cs = last_check = modes_at_read = crop_from = cur_disc = swap_pending = played = drive_on = changed = sig_at = 0;
				script_end = settle = -1;
				memset(emu_host_keys, 0, sizeof emu_host_keys);
				set_capture(0);
				script_cs = boot_script();
			}
			splash_until = SDL_GetTicks() + 2000;       /* the boot behind the splash, for 2 seconds at the least */
			snprintf(text, sizeof text, "A new game, saved to Slot %d", k + 1);
			notify(text);
		}
		if (want_save && !booting && !emu_mute && state_ready() && !state_writing())
		{
			char text[64];
			int failed = save_slot(want_save - 1);

			if (failed)             /* otherwise it is said once the file is written, below */
			{
				snprintf(text, sizeof text, "Slot %d couldn't be saved", want_save);
				notify(text);
			}
			saving_note = !failed;
			armed |= !failed && want_save == game.autosave;
			want_save = 0;
		}
		if (!booting && emu_mute && (Sint32)(SDL_GetTicks() - splash_until) >= (blur_tex ? FADE_MS : 0))      /* the game is going: from here it is shown, and runs in real time */
		{
			if (blur_tex)
				SDL_DestroyTexture(blur_tex);
			blur_tex = NULL;
			emu_audio_fade_in(1000);
			shown = SDL_GetPerformanceCounter();
			shown_cs = cs;
			emu_mute = 0;
			start = SDL_GetPerformanceCounter() - (Uint64)cs * freq / 100;
			if (!fullscreen && !own_size())
				fit_window();
			auto_at = cs + 3000;
			if (!selector)
				waiting_note();
		}
		/* Auto Save: the machine to its slot every 30 seconds of play, written on a thread of its own */
		if (!booting && !emu_mute && game.autosave && armed && cs >= auto_at && state_ready() && !state_writing())
		{
			save_slot(game.autosave - 1);
			auto_at = cs + 3000;
		}
		if (writing_slot >= 0 && !state_writing())
		{
			char text[64];

			if (state_write_failed())       /* a full disc, or a folder that can't be written to: Auto Save's too */
				snprintf(text, sizeof text, "Slot %d couldn't be saved", writing_slot + 1);
			else
				snprintf(text, sizeof text, "Saved to Slot %d", writing_slot + 1);
			if (saving_note || state_write_failed())
				notify(text);
			saving_note = 0;
			saved_at[writing_slot] = slot_time(writing_slot);
			writing_slot = -1;
		}

		if (!booting && cs - last_check >= 10)  /* 10 times a second: is this a screen to switch disks on? */
		{
			last_check = cs;
			swaps_check();
		}

		/* The menus: the system's bar is kept to what they hold, and says what was chosen from it */
		n_items = menu_items(items, booting);
		if (native_bar)
		{
			int cmd;

			if (memcmp(items, bar_items, sizeof items))
			{
				menubar_set(items, n_items);
				memcpy(bar_items, items, sizeof items);
			}
			SDL_GetWindowSize(rg_win, &w, &h);
			menubar_width(w);
			cmd = menubar_poll();
			if (cmd && !(booting && cmd >= CMD_SET_SWAP && cmd < CMD_AUTO))   /* the disk stays in while the game boots */
				quit |= menu_command(cmd);
		}

		SDL_SetRenderDrawColor(rg_ren, 0, 0, 0, 255);
		SDL_RenderClear(rg_ren);
		if (blur_tex && !booting && emu_mute && (Sint32)(SDL_GetTicks() - splash_until) >= 0)
		{
			int gone = (Sint32)(SDL_GetTicks() - splash_until);    /* the blurred picture clearing to the sharp one */

			draw_picture();
			draw_blur(gone < FADE_MS ? 255 - 255 * gone / FADE_MS : 0);
		}
		else if (booting || emu_mute)
		{
			int boot = booting ? 1000 * (cs < script_cs + 150 ? cs : script_cs + 150) / (script_cs + 150) : 1000;
			int left = (Sint32)(splash_until - SDL_GetTicks()), time = left > 0 ? 1000 - left / 2 : 1000;

			if (blur_tex && !booting)
				draw_blur(255);
			draw_splash(boot < time ? boot : time, 1000);
		}
		else
		{
			draw_picture();
			if (paused)
				draw_paused();
		}
		if (selector)
			draw_selector();
		if (confirming)
			draw_confirm();
		draw_note();
		if (shot)
			rg_save_shot(shot);
		SDL_RenderPresent(rg_ren);
		drawn += !booting && !emu_mute;
		if (frame_ticks)        /* without vsync: the next frame a refresh after this one's turn */
		{
			Uint64 now = SDL_GetPerformanceCounter();

			next_frame += frame_ticks;
			if (now >= next_frame + frame_ticks)
				next_frame = now;       /* a frame or more behind: carry on from now */
			else if (next_frame > now)
				SDL_Delay((Uint32)((next_frame - now) * 1000 / freq));
		}
	}

	if (run_for)            /* PIARCBOOT_RUN_FOR=<seconds>: how well the machine kept up, for measuring a host */
		fprintf(stderr, "splash %d ms, then %d cs emulated in %d ms with %d cs lost, %d frames drawn%c", (int)((shown - began) * 1000 / freq),
			cs - shown_cs, (int)((SDL_GetPerformanceCounter() - shown) * 1000 / freq), lost, drawn, 10);
	set_capture(0);
	{
		game_t saved;

		if (!store_load(game.id, &saved) && (saved.fullscreen != fullscreen || saved.win_w != game.win_w || saved.win_h != game.win_h))
		{
			saved.fullscreen = fullscreen;
			saved.win_w = game.win_w;       /* kept as the game closes, not at each step of a drag */
			saved.win_h = game.win_h;
			store_save(&saved);
		}
	}
	while (state_writing())         /* a save still being written is finished first */
		SDL_Delay(5);
	machine_close();
	SDL_Quit();
	return 0;
}
