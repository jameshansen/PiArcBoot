/* The game window: one game, booted hands-free and shown in a window with the system's menu bar.

   A game starts behind a splash screen. The machine is run as fast as the host allows while a
   script holds Shift for a disc that boots, or ROO311's HostLink module runs the game's
   application for one that doesn't, and the splash gives way to the picture once the game has
   put up a screen of its own, so the desktop is never seen. From there the machine runs in real
   time.

   The Game menu has full screen and the three window sizes. Switch Floppy, there only for a game
   with more than one disc, changes the disc in the drive, and an auto switch point remembers the
   screen a game asks for a disc on, so that the next time the game shows that screen the disc
   changes by itself. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

#define MENUS 2
#define MAX_ITEMS (MAX_DISCS + 8)
static const char *const menu_titles[MENUS] = { "Game", "Switch Floppy" };
static int n_menus = MENUS;             /* 1 for a game with one disc: no Switch Floppy */
enum { CMD_FULLSCREEN = 1, CMD_SCALE, CMD_CLOSE = CMD_SCALE + 4, CMD_SET_SWAP, CMD_CLEAR_SWAPS, CMD_SEND_ESC, CMD_CROP, CMD_DISC };
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
   out too. With no windows the picture is shown whole. */
static SDL_Rect view(void)
{
	SDL_Rect v = { 0, 0, emu_w, emu_h };
	int ys = emu_dbl ? 4 : 2, sh = emu_h * ys, x0 = 1280, y0 = sh, x1 = 0, y1 = 0, i;

	for (i = 0; crop && crop_from && now_cs >= crop_from && i < emu_host_nwins; i++)
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

/* The window's size in real pixels for a scale of 1 to 3: the picture, with the menu bar above it */
static void window_size(int *w, int *h)
{
	SDL_Rect v = view();
	int dw = emu_w > 0 ? v.w : 640, dh = emu_h > 0 ? (emu_dbl ? v.h * 2 : v.h) : 512;

	*w = dw * game.scale / 2 * pic_scale();
	*h = dh * game.scale / 2 * pic_scale() + bar_px();
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

/* Full screen has no menu bar, and Alt+Enter comes back. Coming back, the window is the size it
   was. */
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

static void set_capture(int on)
{
	captured = on;
	SDL_SetRelativeMouseMode(on ? SDL_TRUE : SDL_FALSE);
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
		if (most < game.scale * pic_scale() / 2.0)
			most = game.scale * pic_scale() / 2.0;
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
   columns and smoothing alone blurs everything. */
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

/* What the 2 menus hold just now, in order. Returns how many items. */
static int menu_items(menu_item_t *it)
{
	int n = 0, i;

	memset(it, 0, sizeof(menu_item_t) * MAX_ITEMS);
#define ITEM(m, c, tick, line, ...) \
	(it[n].menu = m, it[n].cmd = c, it[n].checked = tick, it[n].rule = line, snprintf(it[n].label, sizeof it[n].label, __VA_ARGS__), n++)
	ITEM(0, CMD_FULLSCREEN, fullscreen, 0, "Fullscreen");
	for (i = 1; i <= 3; i++)
		ITEM(0, CMD_SCALE + i, !fullscreen && game.scale == i && !own_size(), 0, "%dx Scale", i);
	ITEM(0, CMD_CROP, crop, 1, "Windows Only");
	ITEM(0, CMD_CLOSE, 0, 1, "Close");
	ITEM(0, CMD_SEND_ESC, 0, 1, "Send ESC");
	for (i = 0; i < game.ndiscs; i++)
		ITEM(1, CMD_DISC + i, i == cur_disc, 0, "Disk %d", disk_number(i));
	ITEM(1, CMD_SET_SWAP, swap_pending, 1, "Set Auto Switch Point");
	if (n_swaps)
		ITEM(1, CMD_CLEAR_SWAPS, 0, 0, "Clear Auto Switch Points");
#undef ITEM
	return n;
}

/* Does what a menu item says. Returns 1 if the game is to close. */
static int menu_command(int cmd)
{
	if (cmd == CMD_FULLSCREEN)
		set_fullscreen(!fullscreen);
	else if (cmd > CMD_SCALE && cmd <= CMD_SCALE + 3)
	{
		game.scale = cmd - CMD_SCALE;
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
	rg_marble(0, 0, w, h);
	rg_image(icon_tex, w / 2 - 64, h / 2 - 110, 128, 128);
	rg_text_centre(RG_DESKTOP_BOLD, 0, h / 2 + 30, w, 20, 0x000000, game.title);
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
	snprintf(text, sizeof text, "Filer_Run %s.%s", app_root(), game.boot);
	emu_host_run(text);
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

int game_main(const char *id, const char *test_script)
{
	char path[1024], cls[160], rom[1024], extrom[1024], hostfs[1024];
	uint8_t icon[128 * 128 * 4], cmos[256];
	int booting = 1, script_cs, cs = 0, script_end = -1, settle = -1, modes_at_read = 0, last_check = 0, w, h, quit = 0, i;
	int run_for = getenv("PIARCBOOT_RUN_FOR") ? atoi(getenv("PIARCBOOT_RUN_FOR")) : 0, lost = 0, shown_cs = 0;
	menu_item_t items[MAX_ITEMS], bar_items[MAX_ITEMS];
	int n_items;
	Uint64 freq, start, began, shown = 0;
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
	if (rg_open(game.title, 320 * game.scale, 256 * game.scale, RG_RESIZABLE, cls, icon))
	{
		fprintf(stderr, "piarcboot: %s\n", SDL_GetError());
		return 1;
	}
	SDL_InitSubSystem(SDL_INIT_AUDIO);      /* a machine with no sound still plays, silently */
	/* The menu bar is the system's own where it has one to give */
	n_menus = game.ndiscs > 1 ? MENUS : 1;
	native_bar = menubar_open(menu_titles, n_menus);
	n_items = menu_items(bar_items);
	if (native_bar)
		menubar_set(bar_items, n_items);
	SDL_SetWindowMinimumSize(rg_win, rg_to_window(320 * pic_scale()), rg_to_window(200 * pic_scale() + bar_px()));
	fit_window();
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
	script_cs = boot_script();
	emu_mute = 1;

	freq = SDL_GetPerformanceFrequency();
	start = began = SDL_GetPerformanceCounter();
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
					int sc = e.key.keysym.scancode, down = e.type == SDL_KEYDOWN;
					int mod = SDL_GetModState();

					if (down && !e.key.repeat && (mod & KMOD_ALT) && sc == SDL_SCANCODE_RETURN)
						set_fullscreen(!fullscreen);
					else if (sc == SDL_SCANCODE_ESCAPE)    /* frees the mouse, and never reaches the machine */
					{
						if (down)
							set_capture(0);
					}
					else if (down && !e.key.repeat && (mod & KMOD_ALT) && sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9)
					{
						for (i = 0; i < game.ndiscs; i++)       /* Alt and a number: that disk */
							if (disk_number(i) == sc - SDL_SCANCODE_1 + 1)
								choose_disc(i);
					}
					else if (sc < 512 && !booting && !((mod & KMOD_ALT) && sc == SDL_SCANCODE_RETURN))
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
				break;
			}
		}
		if (test_script)
			quit |= rg_test_step(cs, &shot, &drop);

		/* A click on the picture gives the game the mouse. A game set not to capture it, or shown
		   cropped to its windows, has the RISC OS pointer put where the host's is over the picture,
		   by ROO311's HostLink, and the buttons pressed there. */
		if ((game.nocapture || crop) && emu_hostlink)
		{
			SDL_Rect r, v = view();
			double zoom;
			int px, py, over;
			Uint32 b;

			picture_rect(&r, &zoom);
			px = v.x + (int)((rg_in.x * rg_scale - r.x) / zoom);
			py = v.y + (int)((rg_in.y * rg_scale - r.y) / zoom) / (emu_dbl ? 2 : 1);
			over = !booting && rg_in.x >= 0 && px >= v.x && px < v.x + v.w && py >= v.y && py < v.y + v.h;
			if (over)       /* the usual modes are 1280 OS units across, and 2 a line, or 4 a doubled one */
				emu_host_pointer(px * 1280 / emu_w, (emu_h - 1 - py) * (emu_dbl ? 4 : 2));
			b = over ? SDL_GetMouseState(NULL, NULL) : 0;
			emu_mouse(0, 0, (b & SDL_BUTTON_LMASK ? 1 : 0) | (b & SDL_BUTTON_RMASK ? 2 : 0) | (b & SDL_BUTTON_MMASK ? 4 : 0));
		}
		else if (!booting && rg_in.pressed && !captured && (fullscreen || rg_in.y * rg_scale >= bar_px()))
			set_capture(1);
		if (captured)
		{
			Uint32 b = SDL_GetMouseState(NULL, NULL);
			int dx = (int)mouse_fx, dy = (int)mouse_fy;

			mouse_fx -= dx;
			mouse_fy -= dy;
			emu_mouse(dx, dy, (b & SDL_BUTTON_LMASK ? 1 : 0) | (b & SDL_BUTTON_RMASK ? 2 : 0) | (b & SDL_BUTTON_MMASK ? 4 : 0));
		}
		else if (!((game.nocapture || crop) && emu_hostlink))
			emu_mouse(0, 0, 0);

		/* Run the machine: flat out behind the splash and in the tests, in real time otherwise */
		if (booting || test_script)
		{
			int limit = test_script && !booting ? 5 : 100000;

			for (i = 0; i < limit; i++)
			{
				emu_step();
				cs++;
				app_events(cs);
				/* The splash comes down once the keys have all been pressed and the game has
				   either put up a screen of its own since it began to load, or stopped loading,
				   and for a game cropped to its windows once the cropping has started */
				if (!modes_at_read && machine_disc_reads)
					modes_at_read = machine_mode_sets + 1;
				if (booting && !emu_script_busy())
				{
					if (script_end < 0)
						script_end = cs;
					if (settle < 0 && (game.boot[0] ? app.ready && !emu_script_busy() && (!crop || (crop_from && cs >= crop_from)) :
							   modes_at_read && (machine_mode_sets >= modes_at_read || !machine_motor_on())))
						settle = cs + 15;
					if ((settle >= 0 && cs >= settle) || cs > script_end + (game.boot[0] ? (app.mode ? 9000 : 6000) : 3000))
					{
						if (test_script)
							fprintf(stderr, "picture shown at %d cs%s\n", cs, settle < 0 ? " (it never set a mode or stopped loading)" : "");
						booting = 0;
						break;
					}
				}
				if (rg_test_next() >= 0 && rg_test_next() <= cs)
					break;
				if ((SDL_GetPerformanceCounter() - frame_start) * 1000 / freq >= 14)
					break;
			}
			if (!booting && emu_mute)
			{
				shown = SDL_GetPerformanceCounter();
				shown_cs = cs;
				emu_mute = 0;
				start = SDL_GetPerformanceCounter() - (Uint64)cs * freq / 100;
				if (!fullscreen && !own_size())
					fit_window();
			}
		}
		else
		{
			int due = (int)((SDL_GetPerformanceCounter() - start) * 100 / freq) - cs;

			if (due > 10)           /* too far behind to catch up: let the time go */
			{
				start += (Uint64)(due - 10) * freq / 100;
				lost += due - 10;
				due = 10;
			}
			if (run_for && (SDL_GetPerformanceCounter() - shown) / freq >= (Uint64)run_for)
				quit = 1;
			for (i = 0; i < due; i++)
			{
				emu_step();
				cs++;
				app_events(cs);
			}
			if (due <= 0)
				SDL_Delay(1);
		}
		if (!booting && cs - last_check >= 10)  /* 10 times a second: is this a screen to switch disks on? */
		{
			last_check = cs;
			swaps_check();
		}

		/* The menus: the system's bar is kept to what they hold, and says what was chosen from it */
		n_items = menu_items(items);
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
			if (cmd && !(booting && cmd >= CMD_SET_SWAP))   /* the disk stays in while the game boots */
				quit |= menu_command(cmd);
		}

		SDL_SetRenderDrawColor(rg_ren, 0, 0, 0, 255);
		SDL_RenderClear(rg_ren);
		if (booting)
			draw_splash(cs, script_cs + 150);
		else
			draw_picture();
		draw_note();
		if (shot)
			rg_save_shot(shot);
		SDL_RenderPresent(rg_ren);
	}

	if (run_for)            /* PIARCBOOT_RUN_FOR=<seconds>: how well the machine kept up, for measuring a host */
		fprintf(stderr, "splash %d ms, then %d cs emulated in %d ms with %d cs lost%c", (int)((shown - began) * 1000 / freq),
			cs - shown_cs, (int)((SDL_GetPerformanceCounter() - shown) * 1000 / freq), lost, 10);
	set_capture(0);
	{
		game_t saved;

		if (!store_load(game.id, &saved) && (saved.scale != game.scale || saved.fullscreen != fullscreen))
		{
			saved.scale = game.scale;
			saved.fullscreen = fullscreen;
			store_save(&saved);
		}
	}
	machine_close();
	SDL_Quit();
	return 0;
}
