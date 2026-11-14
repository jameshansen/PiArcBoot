/* The game window: one game, booted hands-free and shown in a window with the system's menu bar.

   A game starts behind a splash screen. The machine is run as fast as the host allows while a
   script holds Shift for a disc that boots, or opens the command line and runs the game's
   application for one that doesn't, and the splash gives way to the picture once the game has
   put up a screen of its own. From there the machine runs in real time.

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
static int native_bar;                  /* the window has a menu bar of the system's own making */
static int fit_w, fit_h;                /* the size the window was last given for a size in the menu */
static int kept_w, kept_h;              /* and its size before it went full screen */

#define MENUS 2
#define MAX_ITEMS (MAX_DISCS + 8)
static const char *const menu_titles[MENUS] = { "Game", "Switch Floppy" };
static int n_menus = MENUS;             /* 1 for a game with one disc: no Switch Floppy */
enum { CMD_FULLSCREEN = 1, CMD_SCALE, CMD_CLOSE = CMD_SCALE + 4, CMD_SET_SWAP, CMD_CLEAR_SWAPS, CMD_SEND_ESC, CMD_DISC };
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

/* The window's size in real pixels for a scale of 1 to 3: the picture, with the menu bar above it */
static void window_size(int *w, int *h)
{
	int dw = emu_w > 0 ? emu_w : 640, dh = emu_h > 0 ? (emu_dbl ? emu_h * 2 : emu_h) : 512;

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

/* Gives the window the size the menu's scale asks for */
static void fit_window(void)
{
	window_size(&fit_w, &fit_h);
	fit_w = rg_to_window(fit_w);
	fit_h = rg_to_window(fit_h);
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
		notify("The mouse belongs to the game now. Escape frees it.");
		told_capture = 1;
	}
}

/* Where the picture goes, in real pixels: the largest that keeps its shape in the window below
   the menu bar, or on the whole screen when full screen, with black around it. Full screen keeps
   to a whole multiple of the 1x size, and so does a window within a hair of one. */
static void picture_rect(SDL_Rect *r, double *zoom)
{
	int ow, oh, top, dw = emu_w, dh = emu_dbl ? emu_h * 2 : emu_h, k;

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
	SDL_Rect src = { 0, 0, emu_w, emu_h }, dst;
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
	SDL_UpdateTexture(pic, &src, emu_fb, EMU_FB_W * 4);
	picture_rect(&dst, &zoom);
	if (dst.w < 1 || dst.h < 1)
		return;
	SDL_SetTextureScaleMode(pic, SDL_ScaleModeNearest);
	if (dst.w % emu_w == 0 && dst.h % emu_h == 0)
	{
		SDL_RenderCopy(rg_ren, pic, &src, &dst);
		return;
	}

	/* How many whole times larger to make it first, each way: as many as fit in its place
	   and in a texture */
	tx = dst.w / emu_w > 1 ? dst.w / emu_w : 1;
	ty = dst.h / emu_h > 1 ? dst.h / emu_h : 1;
	if (!SDL_GetRendererInfo(rg_ren, &info) && info.max_texture_width > 0 && info.max_texture_height > 0)
	{
		while (tx > 1 && emu_w * tx > info.max_texture_width)
			tx--;
		while (ty > 1 && emu_h * ty > info.max_texture_height)
			ty--;
	}
	if (tx > 1 || ty > 1)
	{
		if (!big || big_w != emu_w * tx || big_h != emu_h * ty)
		{
			if (big)
				SDL_DestroyTexture(big);
			big_w = emu_w * tx;
			big_h = emu_h * ty;
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
   other disc gets the command line once the desktop has settled (its banner is gone 9.3 seconds
   in), the disc's root opened as a double-click would, and the application run. */
/* A game from the Classics tab is started by ROO311's HostLink module once the desktop is up, as a
   double-click would, and its icon on the icon bar is then clicked for it, every 2 seconds until
   the icon is there or 6 tries have found none (a game that opens a window of its own has none) */
static int click_at = -1, click_tries, modes_at_click = -1, clicked_cs;

static void classic_clicks(int cs)
{
	if (click_at < 0 || cs < click_at)
		return;
	if (emu_host_clicked)
	{
		click_at = -1;
		modes_at_click = emu_host_modes;
		clicked_cs = cs;
	}
	else if (click_tries++ < 6)
	{
		emu_host_click(game.boot);
		click_at = cs + 200;
	}
	else
		click_at = -1;
	if (click_at < 0)               /* the game is going: the disc's directory, opened to start it, goes */
		emu_host_run("Filer_CloseDir adfs::0.$");
}

/* Whether a Classics game has got going: a game that takes the screen has changed mode since
   HostLink started it or its icon was clicked, one that plays in the desktop has had 3 seconds to
   open its window, and one with no icon to click has stopped loading */
static int classic_ready(int cs)
{
	if (emu_host_modes_at_run >= 0 && (emu_host_modes > emu_host_modes_at_run || machine_mode_sets > emu_host_vidc_at_run))
		return 1;
	if (modes_at_click >= 0)
		return emu_host_modes > modes_at_click || cs > clicked_cs + 300;
	return click_at < 0 && !machine_motor_on();
}

static int boot_script(void)
{
	char text[512];

	if (!game.boot[0])
	{
		emu_script("key shift down\nuntilread 600\nkey shift up\n");
		return 270;
	}
	if (game.classic)
	{
		emu_host_run("Filer_OpenDir adfs::0.$");
		snprintf(text, sizeof text, "Filer_Run adfs::0.$.%s", game.boot);
		emu_host_run(text);
		click_at = 1000;
		return 1500;
	}
	snprintf(text, sizeof text, "wait 1200\nkey f12 down\nwait 10\nkey f12 up\nwait 100\n"
		 "type *Filer_OpenDir adfs::0.$\\n\nwait 100\ntype *Filer_Run adfs::0.$.%s\\n\nwait 50\ntype \\n\n", game.boot);
	emu_script(text);
	return 1200 + 14 + 100 + 28 * 11 + 100 + (int)(24 + strlen(game.boot)) * 11 + 50 + 11;
}

int game_main(const char *id, const char *test_script)
{
	char path[1024], cls[160], rom[1024];
	uint8_t icon[128 * 128 * 4];
	int booting = 1, script_cs, cs = 0, script_end = -1, settle = -1, modes_at_read = 0, last_check = 0, w, h, quit = 0, i;
	int run_for = getenv("PIARCBOOT_RUN_FOR") ? atoi(getenv("PIARCBOOT_RUN_FOR")) : 0, lost = 0, shown_cs = 0;
	menu_item_t items[MAX_ITEMS], bar_items[MAX_ITEMS];
	int n_items;
	Uint64 freq, start, began, shown = 0;
	size_t n;

	if (store_load(id, &game) || !game.ndiscs)
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
	fit_window();                   /* room for the menu bar above the picture */
	SDL_SetWindowMinimumSize(rg_win, rg_to_window(320 * pic_scale()), rg_to_window(200 * pic_scale() + bar_px()));
	fit_window();
	icon_tex = rg_image_texture(icon, 128, 128, 1);
	if (game.fullscreen)
		set_fullscreen(1);

	machine_fixed_time = test_script != NULL;
	if (emu_start(rom, default_cmos, game.fdc))
	{
		fprintf(stderr, "piarcboot: the ROM image %s can't be read\n", rom);
		return 1;
	}
	if (!test_script)
		emu_audio_open();
	else
		rg_test_load(test_script);
	store_game_path(game.id, game.disc[0], path, sizeof path);
	machine_disc(0, path, game.ro[0]);
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

		/* A click on the picture gives the game the mouse. A game set not to capture it has the
		   RISC OS pointer put where the host's is over the picture, by ROO311's HostLink, and the
		   buttons pressed there. */
		if (game.nocapture && emu_hostlink)
		{
			SDL_Rect r;
			double zoom;
			int px, py, over;
			Uint32 b;

			picture_rect(&r, &zoom);
			px = (int)((rg_in.x * rg_scale - r.x) / zoom);
			py = (int)((rg_in.y * rg_scale - r.y) / zoom) / (emu_dbl ? 2 : 1);
			over = !booting && rg_in.x >= 0 && px >= 0 && px < emu_w && py >= 0 && py < emu_h;
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
		else if (!(game.nocapture && emu_hostlink))
			emu_mouse(0, 0, 0);

		/* Run the machine: flat out behind the splash and in the tests, in real time otherwise */
		if (booting || test_script)
		{
			int limit = test_script && !booting ? 5 : 100000;

			for (i = 0; i < limit; i++)
			{
				emu_step();
				cs++;
				classic_clicks(cs);
				/* The splash comes down once the keys have all been pressed and the game has
				   either put up a screen of its own since it began to load, or stopped loading */
				if (!modes_at_read && machine_disc_reads)
					modes_at_read = machine_mode_sets + 1;
				if (booting && !emu_script_busy())
				{
					if (script_end < 0)
						script_end = cs;
					if (settle < 0 && (game.classic ? classic_ready(cs) :
							   modes_at_read && (machine_mode_sets >= modes_at_read || !machine_motor_on())))
						settle = cs + 15;
					if ((settle >= 0 && cs >= settle) || cs > script_end + (game.classic ? 6000 : 3000))   /* a Classics game says when it has started */
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
				classic_clicks(cs);
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
