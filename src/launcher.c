/* The launcher: the window with the games in it.

   The JASPP tab lists the games JASPP publishes that ROO311 starts, the installed ones first,
   each with its icon. Install fetches a game and offers shortcuts, Settings changes those and
   removes the game, and Play starts it in a window of its own. The My Games tab holds games made
   from disc images of the user's own, added with Add Game or by dropping the images on the
   window; their Settings also name the game, choose how it starts and pick its icon from the
   sprites on its discs.

   It is drawn with ROOGUI as a RISC OS application: the list is a window's work area with its
   scroll bar, the buttons are action buttons, and the dialogue boxes are windows with a cream
   title bar, where the button Return presses stands in the default button's surround. What it
   has to say comes in a box as Wimp_ReportError puts one up.

   From the keyboard, Ctrl+Tab and Ctrl+Shift+Tab change tabs, Up and Down move a highlight
   through the games, and Return presses the highlighted game's Play or Install. */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <SDL.h>
#include "piarcboot.h"

#define ROW_H 92
#define BTN_W 90
#define HEADING rg_font(RG_BOLD, 14 * 16)       /* Installed and Available, over the games or their sources */
#define BTN_H 26                /* an action button, 52 OS units high */
#define DLG_W 560
#define BAR_W 20                /* width of the scroll bar */
#define MAX_THUMBS 24
#define TAB_W 122
#define TABS_Y 46               /* the top of the tab in front, under the search field */
#define JASPP_URL "https://forums.jaspp.org.uk/"
#define PIARCBOOT_URL "https://github.com/jameshansen/PiArcBoot"

enum { DLG_NONE, DLG_INSTALL, DLG_PROGRESS, DLG_SETTINGS, DLG_MESSAGE, DLG_CONFIRM };
enum { TAB_JASPP, TAB_CLASSICS, TAB_MINE, TAB_ABOUT, TABS };
static const char *const tab_names[TABS] = { "JASPP", "Classics", "My Games", "About" };

static game_t *games;
static int n_games;
static int tab;
static int scroll[TABS], content_h[TABS];
static int quit;
static SDL_Texture *app_tex;            /* PiArcBoot's own icon, for the About tab and the corner over the tabs */
static char search[64];                 /* what the search field holds: each tab shows only the games that match it */
static int source_focus;                /* the caret is in the field for a new PackMan source, not the search field */
static int sel[TABS] = { -1, -1, -1, -1 };     /* the game row highlighted from the keyboard in each tab, counting down the list, or -1 */
static int rows[TABS];                  /* how many game rows each tab showed when it was last drawn */
static int row_n, row_sel_y, reveal, enter;    /* rows counted as the list is drawn, where the highlighted one came, and the keys for it this frame */

/* Whether a game's title or its line of information holds the search, in any case */
static int matches(const char *title, const char *info)
{
	const char *const text[2] = { title, info };
	int t;
	size_t i, k;

	if (!search[0])
		return 1;
	for (t = 0; t < 2; t++)
		for (i = 0; text[t] && text[t][i]; i++)
		{
			for (k = 0; search[k] && tolower((unsigned char)text[t][i + k]) == tolower((unsigned char)search[k]); k++);
			if (!search[k])
				return 1;
		}
	return 0;
}

static int dlg;
static entry_t dlg_entry;
static game_t dlg_game;
static char dlg_text[400], dlg_name[128];
static int dlg_kind;
static int dlg_desktop, dlg_menu;
static char dlg_choices[40][64];
static int dlg_nchoices, dlg_choice;
static int dlg_ro[MAX_DISCS], dlg_changed[MAX_DISCS], dlg_capture, dlg_a3000, dlg_real_drive, dlg_hide_drive, dlg_crop;
static char dlg_image[MAX_DISCS][64];   /* each disk's image format */
static sprite_t *dlg_sprites;
static int dlg_nsprites, dlg_sprite;
static SDL_Texture *dlg_thumbs[MAX_THUMBS];
static int dlg_disc = -1;               /* the disk the confirm box asks about removing, or -1 for the whole game */
static int dlg_scroll, dlg_fitted;      /* how far the settings box's contents are scrolled, and that the window has been made to fit it */
static progress_t progress;
static int stopping, closing;           /* the install was cancelled, and the window was closed while it was going */
static volatile int feed_arrived;

/* Icons already turned into pictures, by game */
static struct { char key[100]; SDL_Texture *t; } icons[512];
static int n_icons;
static SDL_Texture *generic_tex;

static void icon_forget(const char *id)
{
	int i;

	for (i = 0; i < n_icons; i++)
		if (!strcmp(icons[i].key + 2, id))
		{
			if (icons[i].t != generic_tex)
				SDL_DestroyTexture(icons[i].t);
			icons[i] = icons[--n_icons];
			i--;
		}
}

/* A game's icon: its own once installed, and the one that comes with PiArcBoot before that */
static SDL_Texture *icon_for(const char *id, const game_t *g)
{
	char key[100], name[160], path[1024];
	uint8_t *px = NULL;
	int i, w = 0, h = 0;

	snprintf(key, sizeof key, "%c:%s", g ? 'g' : 'c', id);
	for (i = 0; i < n_icons; i++)
		if (!strcmp(icons[i].key, key))
			return icons[i].t;
	if (g)
		store_game_path(id, "icon.png", path, sizeof path);
	else
	{
		snprintf(name, sizeof name, "icons/%s.png", id);
		if (!store_asset(name, path, sizeof path))
			path[0] = 0;
	}
	if (path[0])
		px = png_read(path, &w, &h);
	if (n_icons == 512)
		return generic_tex;
	snprintf(icons[n_icons].key, sizeof icons[0].key, "%s", key);
	icons[n_icons].t = px && w == 128 && h == 128 ? rg_image_texture(px, 128, 128, 1) : generic_tex;
	free(px);
	return icons[n_icons++].t;
}

static void reload_games(void)
{
	free(games);
	n_games = store_list(&games);
}

static game_t *installed(const char *id)
{
	int i;

	for (i = 0; i < n_games; i++)
		if (!strcmp(games[i].id, id))
			return &games[i];
	return NULL;
}

/* Something to tell the user, kind being RG_INFORMATION, or RG_ERROR for what went wrong */
static void message(const char *text, int kind)
{
	snprintf(dlg_text, sizeof dlg_text, "%s", text);
	dlg_kind = kind;
	dlg = DLG_MESSAGE;
}

/* The button Return presses. Its surround goes round the outside of the box given, so the
   button itself lines up with the plain ones beside it. */
static int default_button(int x, int y, int w, int h, const char *label, int enabled)
{
	return rg_default_button(x - 4, y - 4, w + 8, h + 8, label, enabled);
}

static void free_sprites(void)
{
	int i;

	for (i = 0; i < MAX_THUMBS; i++)
		if (dlg_thumbs[i])
		{
			SDL_DestroyTexture(dlg_thumbs[i]);
			dlg_thumbs[i] = NULL;
		}
	sprites_free(dlg_sprites, dlg_nsprites);
	dlg_sprites = NULL;
	dlg_nsprites = 0;
	dlg_sprite = -1;
}

static void close_settings(void)
{
	free_sprites();
	dlg = DLG_NONE;
}

/* Finds afresh what the discs of the game in the settings box offer: the applications it can
   start by, which are on its first disc, and with sprites set, the sprites its icon can be made
   from, each name once. What was chosen from them stays chosen where it is still there. */
static void scan_discs(int sprites)
{
	char boot[64], sprite[16];
	uint8_t *rgba;
	int i, k, n;

	snprintf(boot, sizeof boot, "%s", dlg_nchoices ? dlg_choices[dlg_choice] : dlg_game.boot);
	snprintf(sprite, sizeof sprite, "%s", dlg_sprite >= 0 ? dlg_sprites[dlg_sprite].name : dlg_game.sprite);
	dlg_nchoices = dlg_choice = 0;
	if (dlg_game.jaspp || dlg_game.classic)
		return;
	dlg_nchoices = store_boot_choices(&dlg_game, dlg_choices, 40);
	for (i = 0; i < dlg_nchoices; i++)
		if (!strcmp(dlg_choices[i], boot))
			dlg_choice = i;
	if (!sprites)
		return;
	free_sprites();
	n = store_sprites(&dlg_game, &dlg_sprites);
	for (i = 0; i < n; i++)         /* a sprite on several discs comes once */
	{
		for (k = 0; k < dlg_nsprites && strcmp(dlg_sprites[k].name, dlg_sprites[i].name); k++);
		if (k < dlg_nsprites)
			free(dlg_sprites[i].rgba);
		else
			dlg_sprites[dlg_nsprites++] = dlg_sprites[i];
	}
	rgba = malloc(128 * 128 * 4);
	for (i = 0; i < dlg_nsprites && i < MAX_THUMBS; i++)
	{
		icon_render(rgba, &dlg_sprites[i]);
		dlg_thumbs[i] = rg_image_texture(rgba, 128, 128, 1);
		if (!strcmp(dlg_sprites[i].name, sprite))
			dlg_sprite = i;
	}
	free(rgba);
}

static void open_settings(const game_t *g)
{
	int i;

	free_sprites();
	dlg_game = *g;
	dlg_desktop = g->desktop;
	dlg_capture = !g->nocapture;
	dlg_a3000 = g->a3000;
	dlg_real_drive = g->real_drive;
	dlg_hide_drive = HIDES_DRIVE(g);
	dlg_crop = CROPS(g);
	dlg_menu = g->menu;
	dlg_scroll = dlg_fitted = 0;
	for (i = 0; i < g->ndiscs; i++)
	{
		const char *dot = strrchr(g->disc[i], '.');
		char path[1024], ext[8] = "";
		struct stat st;
		size_t k;

		dlg_ro[i] = g->ro[i];
		dlg_changed[i] = store_disc_changed(g, i);
		/* the image's format, and for a flat image the size of the disc */
		for (k = 0; dot && dot[k + 1] && k < sizeof ext - 1; k++)
			ext[k] = (char)toupper((unsigned char)dot[k + 1]);
		ext[k] = 0;
		store_game_path(g->id, g->disc[i], path, sizeof path);
		if (stat(path, &st) || !strcmp(ext, "JFD") || !strcmp(ext, "HFE"))
			snprintf(dlg_image[i], sizeof dlg_image[i], "%s image", ext);
		else if (st.st_size >= 1600 * 1024)
			snprintf(dlg_image[i], sizeof dlg_image[i], "%s, 1.6MB", ext);
		else
			snprintf(dlg_image[i], sizeof dlg_image[i], "%s, %dK", ext, (int)(st.st_size / 1024));
	}
	snprintf(dlg_name, sizeof dlg_name, "%s", g->title);
	dlg_nchoices = 0;
	scan_discs(1);
	dlg = DLG_SETTINGS;
}

/* Exchanges two entries of an array, each size bytes and no more than 64 */
static void swap_entries(void *array, size_t size, int a, int b)
{
	char t[64], *p = array;

	memcpy(t, p + a * size, size);
	memcpy(p + a * size, p + b * size, size);
	memcpy(p + b * size, t, size);
}

/* Takes entry n out of an array of count entries, each size bytes, closing the gap */
static void drop_entry(void *array, size_t size, int n, int count)
{
	char *p = array;

	memmove(p + n * size, p + (n + 1) * size, (size_t)(count - n - 1) * size);
}

/* Moves a disk of the game in the settings box to another place in its list, with what the box
   shows about it. The order is saved with OK. */
static void swap_discs(int a, int b)
{
	swap_entries(dlg_game.disc, sizeof dlg_game.disc[0], a, b);
	swap_entries(dlg_ro, sizeof dlg_ro[0], a, b);
	swap_entries(dlg_changed, sizeof dlg_changed[0], a, b);
	swap_entries(dlg_image, sizeof dlg_image[0], a, b);
	if (!a || !b)
		scan_discs(0);          /* the game starts from what is on its first disk */
}

/* The points the game window learnt to change disks at name the disks by their places in the
   list, so they go when the places change */
static void forget_swaps(const char *id)
{
	char path[1100];

	store_game_path(id, "swaps.bin", path, sizeof path);
	remove(path);
}

/* Takes disk n out of the game in the settings box. Its image and what the game saved to it are
   deleted and the game is saved without it at once; the box keeps its other changes. */
static void remove_disc(int n)
{
	char path[1100];
	game_t g;
	int i, k;

	if (!store_load(dlg_game.id, &g))
	{
		for (i = k = 0; i < g.ndiscs; i++)
			if (strcmp(g.disc[i], dlg_game.disc[n]))
			{
				memmove(g.disc[k], g.disc[i], sizeof g.disc[0]);
				g.ro[k++] = g.ro[i];
			}
		g.ndiscs = k;
		store_save(&g);
	}
	store_game_path(dlg_game.id, dlg_game.disc[n], path, sizeof path);
	remove(path);
	snprintf(path + strlen(path), sizeof path - strlen(path), ".sav");
	remove(path);
	forget_swaps(dlg_game.id);
	drop_entry(dlg_game.disc, sizeof dlg_game.disc[0], n, dlg_game.ndiscs);
	drop_entry(dlg_ro, sizeof dlg_ro[0], n, dlg_game.ndiscs);
	drop_entry(dlg_changed, sizeof dlg_changed[0], n, dlg_game.ndiscs);
	drop_entry(dlg_image, sizeof dlg_image[0], n, dlg_game.ndiscs);
	dlg_game.ndiscs--;
	scan_discs(1);
	reload_games();
}

/* The game the install box is for: from the JASPP list, or with dlg_classic set, the Classics tab */
static int dlg_classic;
static classic_t dlg_classic_entry;

static const char *install_id(void)
{
	return dlg_classic ? dlg_classic_entry.id : dlg_entry.id;
}

static int install_thread(void *p)
{
	(void)p;
	if (dlg_classic)
		install_classic(&dlg_classic_entry, dlg_desktop, dlg_menu, &progress);
	else
		install_jaspp(&dlg_entry, dlg_desktop, dlg_menu, &progress);
	return 0;
}

static char setup_id[80], setup_title[128];    /* a game added by hand whose installer is running */

static int setup_thread(void *p)
{
	game_t g;

	(void)p;
	if (store_load(setup_id, &g) || run_installer(&g, g.id, &progress) < 0)
		progress.done = -1;
	else
	{
		store_save(&g);
		progress.done = 1;
	}
	return 0;
}

/* A game added by hand that its presets give an installer has it run, as a JASPP game's is when
   it installs. 1 if it has started. */
static int start_setup(const char *id)
{
	char path[1100];
	game_t g;

	store_game_path(id, "installed", path, sizeof path);
	if (store_load(id, &g) || is_dir(path) || !has_installer(&g, id))
		return 0;
	snprintf(setup_id, sizeof setup_id, "%s", id);
	snprintf(setup_title, sizeof setup_title, "%s", g.title);
	memset(&progress, 0, sizeof progress);
	progress.percent = -1;
	snprintf(progress.step, sizeof progress.step, "Getting ready...");
	snprintf(progress.error, sizeof progress.error, "The game's installer couldn't run.");
	stopping = 0;
	dlg = DLG_PROGRESS;
	SDL_DetachThread(SDL_CreateThread(setup_thread, "setup", NULL));
	return 1;
}

/* Brings the feed up to date, then finds out which of the games new to it have floppy images,
   and fetches the PackMan sources' indexes for the Classics tab */
static int feed_thread(void *p)
{
	(void)p;
	if (!feed_refresh())
		feed_arrived = 1;
	if (feed_check_new())
		feed_arrived = 1;
	if (classics_refresh())
		feed_arrived = 1;
	return 0;
}

static void load_feed(void)
{
	char path[1024];
	char *text;

	snprintf(path, sizeof path, "%s/feed.txt", store_dir());
	text = (char *)file_read(path, NULL);
	if (text)
		catalog_load_feed(text);
	free(text);
	classics_load();
}

static void add_files(char **files, int n)
{
	char id[80], error[240];
	game_t g;

	if (n <= 0)
		return;
	if (dlg == DLG_SETTINGS && !dlg_game.jaspp && !dlg_game.classic)     /* dropped on a game's settings: more discs for it */
	{
		if (import_game(files, n, dlg_game.id, NULL, error, sizeof error))
		{
			close_settings();
			message(error, RG_ERROR);
			return;
		}
		close_settings();
		reload_games();
		icon_forget(dlg_game.id);
		if (error[0])           /* added, with a word on what was left out */
			message(error, RG_INFORMATION);
		else if (!start_setup(dlg_game.id) && !store_load(dlg_game.id, &g))
			open_settings(&g);
		return;
	}
	if (dlg != DLG_NONE)
		return;
	if (import_game(files, n, NULL, id, error, sizeof error))
	{
		message(error, RG_ERROR);
		return;
	}
	reload_games();
	tab = TAB_MINE;
	if (error[0])                   /* added, with a word on what was left out */
		message(error, RG_INFORMATION);
	else if (!start_setup(id) && !store_load(id, &g))
		open_settings(&g);              /* a new game opens on its settings, to be named and given its icon */
}

/* Counts a game row as the list is drawn at y. Returns 1 for the one highlighted from the keyboard. */
static int next_row(int y)
{
	if (row_n == sel[tab])
		row_sel_y = y;
	return row_n++ == sel[tab];
}

/* One game's row, a slab with the icon at its left and the buttons at its right, filled with
   cream, the colour of the input focus, when it is highlighted from the keyboard. Returns 1 for
   its default button (Play, or Install), 2 for Settings. */
static int draw_row(int x, int y, int w, SDL_Texture *icon, const char *title, const char *info,
		    const game_t *g, int is_installed, int lit)
{
	int hit = 0, bx = x + w - 22 - BTN_W, by = y + (ROW_H - BTN_H) / 2, i;
	int text_w = w - 104 - (is_installed ? 2 * BTN_W + 56 : BTN_W + 40);
	char label[32];

	if (lit)
		rg_fill(x, y, w, ROW_H, rg_colour(RG_CREAM));
	rg_bevel(x, y, w, ROW_H, 0);
	rg_image(icon, x + 10, y + 8, 76, 76);
	rg_text_fit(RG_DESKTOP, x + 102, y + 19, text_w, rg_colour(7), title);
	if (g && !g->jaspp && !g->classic)
	{
		/* a label for each disk, as many as there is room for before the buttons, the last
		   place going to how many more there are when they don't all fit */
		int room = text_w + 2, shown = g->ndiscs;

		snprintf(label, sizeof label, "+%d more", g->ndiscs);
		if (g->ndiscs * 62 - 4 > room)
			for (shown = 0; (shown + 1) * 62 + rg_text_w(RG_DESKTOP, label) <= room; shown++);
		for (i = 0; i < shown; i++)
		{
			snprintf(label, sizeof label, "Disk %d", i + 1);
			rg_display(x + 100 + i * 62, y + 45, 58, 24, label);
		}
		if (shown < g->ndiscs)
		{
			snprintf(label, sizeof label, "+%d more", g->ndiscs - shown);
			rg_text_centre(RG_DESKTOP, x + 100 + shown * 62, y + 45, rg_text_w(RG_DESKTOP, label), 24, rg_colour(7), label);
		}
	}
	else
		rg_text_fit(SMALL, x + 102, y + 45, text_w, rg_colour(6), info);
	if (is_installed)
	{
		if (default_button(bx, by, BTN_W, BTN_H, "Play", 1))
			hit = 1;
		if (rg_button(bx - BTN_W - 18, by, BTN_W, BTN_H, "Settings", 1))
			hit = 2;
	}
	else if (default_button(bx, by, BTN_W, BTN_H, "Install", 1))
		hit = 1;
	return hit;
}

/* The number a disk goes by: for one of the user's own games its place in the list, which can be
   changed, and otherwise the one in its file's name (disc4.jfd is Disk 4), a JASPP game's number
   in its set */
static int disk_number(const game_t *g, int n)
{
	int number = 0;

	if (!g->jaspp && !g->classic)
		return n + 1;
	return sscanf(g->disc[n], "disc%d", &number) == 1 && number > 0 ? number : n + 1;
}

/* Starts a game, or says what keeps it from starting: settings that can't be read or name no
   disc images, or an image that is missing or empty */
static void play(const game_t *listed)
{
	const char *name = listed->title[0] ? listed->title : listed->id;
	const char *again = listed->jaspp || listed->classic ? "Uninstall it and install it again." : "Remove it and add it again.";
	char path[1100], text[400] = "";
	struct stat st;
	game_t g;
	int i;

	if (store_load(listed->id, &g))
		snprintf(text, sizeof text, "%s can't be started, as its settings file, game.ini, can't be read. %s", name, again);
	else if (!g.ndiscs)     /* a game with no discs is in a HostFS folder, or in its installer's copy of one */
	{
		store_game_path(g.id, "hostfs", path, sizeof path);
		if (!is_dir(path) && (store_game_path(g.id, "installed", path, sizeof path), !is_dir(path)))
			snprintf(text, sizeof text, "%s can't be started, as its settings file, game.ini, names no disc images: it is empty or damaged. %s", name, again);
	}
	for (i = 0; !text[0] && i < g.ndiscs; i++)
	{
		store_game_path(g.id, g.disc[i], path, sizeof path);
		if (stat(path, &st) || !st.st_size)
			snprintf(text, sizeof text, "%s can't be started, as the image of its disk %d, %s, is %s. %s", name, disk_number(&g, i), g.disc[i],
				 stat(path, &st) ? "missing from its folder" : "empty", again);
	}
	if (!text[0] && spawn_game(listed->id))
		snprintf(text, sizeof text, "The game couldn't be started.");
	if (text[0])
		message(text, RG_ERROR);
}

static int over_link;            /* the pointer is over a link in what was just drawn */

/* Text that opens a web page when it is clicked, underlined in the Wimp's dark blue, with the
   pointer a hand over it. Returns its width. */
static int link(int font, int x, int y, const char *text, const char *url)
{
	int w = rg_text_w(font, text), h = rg_font_height(font);

	rg_text(font, x, y, rg_colour(RG_DARK_BLUE), text);
	rg_fill(x, y + rg_font_ascent(font) + 2, w, 1, rg_colour(RG_DARK_BLUE));
	if (rg_over(x, y, w, h))
	{
		over_link = 1;
		if (rg_in.released)
			open_url(url);
	}
	return w;
}

/* A line saying where a list's games come from, with the source's name a link. Returns its height. */
static int credit(int x, int y, int w, const char *lead, const char *name, const char *url)
{
	int lw = rg_text_w(RG_DESKTOP, lead);

	rg_text(RG_DESKTOP, x, y, rg_colour(7), lead);
	if (lw + rg_text_w(RG_DESKTOP, name) <= w)
	{
		link(RG_DESKTOP, x + lw, y, name, url);
		return 30;
	}
	link(RG_DESKTOP, x, y + 22, name, url);  /* a narrow window: the name goes under */
	return 52;
}

/* Text broken at spaces to lines no wider than w, drawn or only measured. Returns the height it takes. */
static int wrap(int font, int x, int y, int w, uint32_t rgb, const char *text, int draw)
{
	char line[256];
	int top = y, step = rg_font_height(font) + 2;

	while (*text)
	{
		size_t len = 0, fit = 0;

		while (text[len] && len < sizeof line - 1)
		{
			size_t end = len + strcspn(text + len, " ");

			if (end >= sizeof line)
				break;
			snprintf(line, sizeof line, "%.*s", (int)end, text);
			if (fit && rg_text_w(font, line) > w)
				break;
			fit = end;
			len = end + (text[end] == ' ');
		}
		snprintf(line, sizeof line, "%.*s", (int)(fit ? fit : len), text);
		if (draw)
			rg_text(font, x, y, rgb, line);
		y += step;
		text += fit ? fit : len;
		while (*text == ' ')
			text++;
	}
	return y - top;
}

/* Who made PiArcBoot and what it is made from, each with its licence and where to find it */
static int draw_about(int x, int y, int w)
{
	static const struct { const char *name, *what, *licence, *url; } parts[] = {
		{ "ArcEm", "The Acorn Archimedes emulator: David Alan Gilbert, Peter Naulls, Peter Howkins, Matthew Howkins, "
		  "Jeffrey Lee, Daniel Clarke, Cameron Cawley and its other contributors, with a processor that began as "
		  "the ARMulator of Advanced RISC Machines Ltd", "GNU General Public License, version 2 or later",
		  "https://github.com/ArcEm-emu/ArcEm" },
		{ "ROO311", "The ROM: RISC OS 3.11 built by James Hansen from the sources RISC OS Open publishes",
		  "Apache License, version 2.0", "https://github.com/jameshansen/ROO311" },
		{ "ROOGUI", "The RISC OS look of the launcher, by James Hansen, with the Window Manager's sprites and the "
		  "Homerton font from the RISC OS Open sources", "Apache License, version 2.0", "https://github.com/jameshansen/ROOGUI" },
		{ "SDL2", "Windows, sound and input: Sam Lantinga and the SDL contributors", "zlib License", "https://www.libsdl.org" },
		{ "zlib", "Compression: Jean-loup Gailly and Mark Adler", "zlib License", "https://zlib.net" },
		{ "GTK 3", "The menu bar on Linux, loaded when a game starts: the GTK team", "GNU Lesser General Public License, "
		  "version 2.1 or later", "https://www.gtk.org" },
		{ "CRT-Royale-Pi", "A shader in the Shaders menu, made from CRT-Royale by TroggleMonkey",
		  "GNU General Public License, version 2 or later", "https://github.com/libretro/glsl-shaders" },
		{ "CRT-Pi", "A shader in the Shaders menu: davej", "GNU General Public License, version 2 or later",
		  "https://github.com/libretro/glsl-shaders" },
		{ "JASPP", "The games, released with their publishers' permission, and the JFD disc image format",
		  "Each game remains its publisher's", JASPP_URL },
	};
	char line[200];
	size_t i;
	int top = y;

	rg_image(app_tex, x + (w - 192) / 2, y, 192, 192);
	y += 204;
	rg_text_centre(RG_DESKTOP_BOLD, x, y, w, 20, rg_colour(7), "PiArcBoot");
	snprintf(line, sizeof line, "Version %s", PIARCBOOT_VERSION);
	rg_text_centre(RG_DESKTOP, x, y + 24, w, 20, rg_colour(7), line);
	rg_text_centre(RG_DESKTOP, x, y + 48, w, 20, rg_colour(7), "Acorn Archimedes games, booted straight from their disc images");
	rg_text_centre(RG_DESKTOP, x, y + 72, w, 20, rg_colour(7), "By James Hansen");
	{
		static const struct { const char *text, *url; } links[] = {
			{ "PiArcBoot on GitHub", PIARCBOOT_URL }, { "Issues", PIARCBOOT_URL "/issues" }, { "Discord", "https://discord.gg/88VzuWSYUc" },
		};
		const char *gpl = "GNU General Public License v3.0";
		int gw = rg_text_w(RG_DESKTOP, gpl), k, lx = x + w / 2 + 16;

		link(RG_DESKTOP, x + (w - gw) / 2, y + 96, gpl, "https://www.gnu.org/licenses/gpl-3.0.html");
		for (k = 0; k < 3; k++)         /* PiArcBoot's own pages, in a row under it */
			lx -= (rg_text_w(RG_DESKTOP, links[k].text) + 32) / 2;
		for (k = 0; k < 3; k++)
			lx += link(RG_DESKTOP, lx, y + 120, links[k].text, links[k].url) + 32;
	}
	y += 164;
	rg_text(RG_DESKTOP_BOLD, x + 2, y, rg_colour(7), "Made with");
	y += 30;
	for (i = 0; i < sizeof parts / sizeof *parts; i++)
	{
		int lw = rg_text_w(SMALL, parts[i].licence), th = wrap(SMALL, 0, 0, w - 24, 0, parts[i].what, 0), h = 64 + th;

		rg_bevel(x, y, w, h, 0);
		rg_text(RG_DESKTOP_BOLD, x + 12, y + 8, rg_colour(7), parts[i].name);
		wrap(SMALL, x + 12, y + 32, w - 24, rg_colour(7), parts[i].what, 1);
		rg_text(SMALL, x + 12, y + 36 + th, rg_colour(6), parts[i].licence);
		link(SMALL, x + 12 + lw + 24, y + 36 + th, parts[i].url, parts[i].url);
		y += h + 8;
	}
	return y - top;
}

static int classics_thread(void *p)
{
	(void)p;
	if (classics_refresh())
		feed_arrived = 1;
	return 0;
}

/* The Classics tab: a section for each source with its line of credit and its games, then the
   PackMan sources, which can be added to and taken away */
static int draw_classics(int x, int y, int w)
{
	static const struct { const char *title, *lead, *name, *url; } sections[CLASSIC_SECTIONS] = {
		{ "Acorn Arcade", "Former commercial games made freeware by their authors, from ", "Acorn Arcade",
		  "http://www.acornarcade.com/articles/Free_games/index879.html" },
		{ "PackMan", "Games for the RISC OS desktop from ", "PackMan's package sources", "https://packages.riscosopen.org/" },
	};
	static char urls[32][300], new_source[300];
	int top = y, s, i, n, pass, any = 0;

	for (pass = 0; pass < 2; pass++)        /* the installed games, then the rest, each under the source it came from */
	{
		int head = 0;

		for (s = 0; s < CLASSIC_SECTIONS; s++)
		{
			int sub = 0, total = 0;

			for (i = 0; i < classic_count(); i++)
				total += classic_entry(i)->section == s;
			/* an available section whose source's list hasn't come yet says so, under its credit */
			if (pass && !total && !search[0])
			{
				if (!head)
				{
					rg_text(HEADING, x + 2, y + 2, rg_colour(7), "Available");
					y += 34;
					head = 1;
				}
				rg_text(RG_DESKTOP_BOLD, x + 2, y + 4, rg_colour(7), sections[s].title);
				y += 26;
				y += credit(x + 2, y + 4, w - 4, sections[s].lead, sections[s].name, sections[s].url);
				rg_text(SMALL, x + 2, y + 2, rg_colour(6), "The source's list arrives once the network is reached.");
				y += 32;
				continue;
			}
			for (i = 0; i < classic_count(); i++)
			{
				const classic_t *c = classic_entry(i);
				game_t *g = installed(c->id);
				int hit, lit;

				if (c->section != s || (g != NULL) != (pass == 0) || !matches(g ? g->title : c->title, g ? g->info : c->info))
					continue;
				if (!head)
				{
					rg_text(HEADING, x + 2, y + 2, rg_colour(7), pass ? "Available" : "Installed");
					y += 34;
					head = 1;
				}
				if (!sub)
				{
					rg_text(RG_DESKTOP_BOLD, x + 2, y + 4, rg_colour(7), sections[s].title);
					y += 26;
					if (pass)
						y += credit(x + 2, y + 4, w - 4, sections[s].lead, sections[s].name, sections[s].url);
					else
						y += 4;
					sub = 1;
				}
				any = 1;
				lit = next_row(y);
				hit = draw_row(x, y, w, icon_for(c->id, g), g ? g->title : c->title, g ? g->info : c->info, g, g != NULL, lit);
				if (lit && enter)
					hit = 1;
				if (hit == 1 && g)
					play(g);
				else if (hit == 1)
				{
					dlg_classic_entry = *c;
					dlg_classic = 1;
					dlg = DLG_INSTALL;
				}
				else if (hit == 2)
					open_settings(g);
				y += ROW_H + 8;
			}
			if (sub)
				y += 4;
		}
		if (head)
			y += 8;
	}
	if (!any && search[0])
	{
		rg_text(SMALL, x + 2, y + 2, rg_colour(6), "None of the Classics match the search.");
		y += 32;
	}

	if (search[0])          /* a search shows the games alone */
		return y - top;
	rg_text(RG_DESKTOP_BOLD, x + 2, y + 4, rg_colour(7), "PackMan sources");
	y += 30;
	n = classics_sources(urls, 32);
	for (i = 0; i < n; i++)
	{
		rg_display(x, y, w - BTN_W - 16, 26, urls[i]);
		if (rg_button(x + w - BTN_W, y, BTN_W, BTN_H, "Remove", 1))
		{
			classics_remove_source(i);
			classics_load();
		}
		y += 34;
	}
	if (rg_in.pressed)
		source_focus = rg_over(x, y, w - BTN_W - 16, 26);       /* a click in it gives it the caret, and anywhere else takes it away */
	rg_field(x, y, w - BTN_W - 16, 26, new_source, sizeof new_source, source_focus);
	if (rg_button(x + w - BTN_W, y, BTN_W, BTN_H, "Add", new_source[0] != 0) || (source_focus && rg_in.key == SDLK_RETURN && new_source[0]))
	{
		char why[400];

		for (i = 0; i < n && strcmp(urls[i], new_source); i++);
		classics_add_source(new_source);
		if (classics_sources(urls, 32) > n)
		{
			new_source[0] = 0;
			SDL_DetachThread(SDL_CreateThread(classics_thread, "classics", NULL));
		}
		else            /* not taken: the field keeps it to be put right */
		{
			if (i < n)
				snprintf(why, sizeof why, "That source is in the list already.");
			else if (n >= 32)
				snprintf(why, sizeof why, "There can be 32 sources at most. Remove one to add another.");
			else
				snprintf(why, sizeof why, "\"%.80s\" can't be added as a source. A source's address begins with http:// or https://, "
					 "and has no spaces, quotes, $ or \\ in it.", new_source);
			message(why, RG_ERROR);
		}
	}
	y += 34;
	rg_text(SMALL, x + 2, y, rg_colour(6), "A source is the address of a PackMan package index, as PackMan's Sources window takes it.");
	return y + 24 - top;
}

/* Whether a game is on My Games: one of the user's own, or a Classics game installed from an entry
   the list has since left out, which can still be played and uninstalled from there. PackMan's
   part of the list comes over the network, so its games stay on Classics. */
static int my_game(const game_t *g)
{
	int i;

	if (g->jaspp || (g->classic && !strncmp(g->id, "pkg-", 4)))
		return 0;
	for (i = 0; g->classic && i < classic_count(); i++)
		if (!strcmp(classic_entry(i)->id, g->id))
			return 0;
	return 1;
}

/* How many games a tab other than About shows for the search */
static int tab_count(int t)
{
	int i, n = 0;

	for (i = 0; t != TAB_CLASSICS && i < n_games; i++)
		n += (t == TAB_JASPP ? games[i].jaspp : my_game(&games[i])) && matches(games[i].title, games[i].info);
	for (i = 0; t == TAB_JASPP && i < catalog_count(); i++)
		n += !installed(catalog_entry(i)->id) && catalog_entry(i)->floppies > 0 && matches(catalog_entry(i)->title, catalog_entry(i)->info);
	for (i = 0; t == TAB_CLASSICS && i < classic_count(); i++)
	{
		const classic_t *c = classic_entry(i);
		const game_t *g = installed(c->id);

		n += matches(g ? g->title : c->title, g ? g->info : c->info);
	}
	return n;
}

static rg_window_t list;        /* the pane the list is in: a work area with its scroll bar */

/* The list under a tab, in a pane that scrolls. vx, vy, vw, vh is the pane's work area; its
   outline and its scroll bar go round that. Returns 1 if the list came out a different height
   from the one the scroll bar was drawn for, when the window is to be drawn again. */
static int draw_list(int vx, int vy, int vw, int vh)
{
	int y, x = vx + 10, w = vw - 20, i, hit, lit, any = 0, before = content_h[tab], moved = 0;
	int px = rg_in.x, py = rg_in.y, wheel = rg_in.wheel;

	if (rg_in.key == SDLK_PAGEDOWN) scroll[tab] += vh - 40;
	if (rg_in.key == SDLK_PAGEUP) scroll[tab] -= vh - 40;
	if (rg_in.key == SDLK_HOME) scroll[tab] = 0;
	if (rg_in.key == SDLK_END) scroll[tab] = content_h[tab];
	if ((rg_in.key == SDLK_DOWN || rg_in.key == SDLK_UP) && rows[tab])     /* the highlight to the next row or the one before */
	{
		sel[tab] = sel[tab] < 0 ? 0 : sel[tab] + (rg_in.key == SDLK_DOWN ? 1 : -1);
		sel[tab] = sel[tab] < 0 ? 0 : sel[tab] >= rows[tab] ? rows[tab] - 1 : sel[tab];
		reveal = 1;
	}
	enter = !source_focus && (rg_in.key == SDLK_RETURN || rg_in.key == SDLK_KP_ENTER);
	row_n = 0;
	if (rg_over(vx, vy, vw + BAR_W, vh))
		scroll[tab] -= wheel * 60;      /* most of a row for each notch of the wheel */
	list.x = vx;
	list.y = vy;
	list.w = list.ex = vw;
	list.h = vh;
	list.ey = content_h[tab] > vh ? content_h[tab] : vh;
	list.sy = scroll[tab];
	list.flags = RG_W_VSCROLL;
	rg_in.wheel = 0;
	rg_window(&list);               /* keeps the list within its ends, and takes the scroll bar's clicks */
	rg_in.wheel = wheel;
	scroll[tab] = list.sy;

	y = vy + 8 - scroll[tab];
	if (!rg_over(vx, vy, vw, vh))
		rg_in.x = rg_in.y = -1;         /* a pointer outside the list touches nothing in it */
	rg_clip(vx, vy, vw, vh);
	if (tab == TAB_ABOUT)
		y += draw_about(x, y, w) + 4;
	else if (tab == TAB_CLASSICS)
		y += draw_classics(x, y, w);
	else if (tab == TAB_JASPP)
	{
		y += credit(x + 2, y + 4, w - 4, "Archimedes Games available from the ", "Archimedes Software Preservation Project", JASPP_URL);
		for (i = 0; i < n_games; i++)
			any |= games[i].jaspp && matches(games[i].title, games[i].info);
		if (any)
		{
			rg_text(HEADING, x + 2, y + 2, rg_colour(7), "Installed");
			y += 34;
			for (i = 0; i < n_games; i++)
				if (games[i].jaspp && matches(games[i].title, games[i].info))
				{
					lit = next_row(y);
					hit = y + ROW_H > vy && y < vy + vh ?
					      draw_row(x, y, w, icon_for(games[i].id, &games[i]), games[i].title, games[i].info, &games[i], 1, lit) : 0;
					if (lit && enter) hit = 1;
					if (hit == 1) play(&games[i]);
					if (hit == 2) open_settings(&games[i]);
					y += ROW_H + 8;
				}
			y += 4;
		}
		for (i = 0; i < catalog_count(); i++)
		{
			const entry_t *e = catalog_entry(i);
			char info[200];

			if (installed(e->id) || e->floppies <= 0 || !matches(e->title, e->info))       /* nothing to install a game from: not listed */
				continue;
			if (any < 2)
			{
				rg_text(HEADING, x + 2, y + 2, rg_colour(7), "Available");
				y += 34;
				any = 2;
			}
			lit = next_row(y);
			hit = lit && enter;
			if (y + ROW_H > vy && y < vy + vh)
			{
				snprintf(info, sizeof info, "%s%s", e->info, e->tested ? "" : e->info[0] ? ", new on JASPP and untested" : "New on JASPP and untested");
				hit |= draw_row(x, y, w, icon_for(e->id, NULL), e->title, info, NULL, 0, lit);
			}
			if (hit)
			{
				dlg_entry = *e;
				dlg_classic = 0;
				dlg = DLG_INSTALL;
			}
			y += ROW_H + 8;
		}
		if (!any && search[0])
		{
			rg_text(SMALL, x + 2, y + 2, rg_colour(6), "No games match the search.");
			y += 24;
		}
	}
	else
	{
		int added = 0;          /* games of the user's own, whatever the search */

		for (i = 0; i < n_games; i++)
			added |= my_game(&games[i]);
		for (i = 0; i < n_games; i++)
			if (my_game(&games[i]) && matches(games[i].title, games[i].info))
			{
				lit = next_row(y);
				hit = y + ROW_H > vy && y < vy + vh ?
				      draw_row(x, y, w, icon_for(games[i].id, &games[i]), games[i].title, games[i].info, &games[i], 1, lit) : 0;
				if (lit && enter) hit = 1;
				if (hit == 1) play(&games[i]);
				if (hit == 2) open_settings(&games[i]);
				y += ROW_H + 8;
				any = 1;
			}
		if (!any && search[0] && added)
		{
			rg_text(SMALL, x + 2, y + 2, rg_colour(6), "None of your games match the search.");
			y += 24;
		}
		y += 14;
		if (rg_button(vx + (vw - 110) / 2, y, 110, BTN_H, "Add Game...", 1))
		{
			int n;
			char **files = pick_files(0, &n);

			if (n < 0)
				message("Drop the game's disc images onto this window to add it.", RG_INFORMATION);
			add_files(files, n);
			while (n-- > 0)
				free(files[n]);
			free(files);
		}
		y += BTN_H + 12;
		rg_text_centre(SMALL, vx, y, vw, 14, rg_colour(6), added ? "Disc images and archives can also be dropped onto this window." :
			       "Add a game from its disc images (JFD, ADF, ADL or HFE) or an archive of its applications (zip, Spark or ArcFS).");
		y += 20;
	}
	rg_unclip();
	rg_in.x = px;
	rg_in.y = py;

	/* the highlight stays on a row the list has, and a row it has just moved to is scrolled into view */
	rows[tab] = row_n;
	if (sel[tab] >= row_n)
		sel[tab] = row_n - 1;
	if (reveal && sel[tab] >= 0)
	{
		if (row_sel_y < vy + 4)
			scroll[tab] -= vy + 4 - row_sel_y;
		else if (row_sel_y + ROW_H > vy + vh - 4)
			scroll[tab] += row_sel_y + ROW_H - (vy + vh - 4);
		moved = 1;
	}
	reveal = 0;

	content_h[tab] = y + scroll[tab] - vy + 4;
	return content_h[tab] != before || moved;
}

/* What the launcher has to say or ask, in a box as Wimp_ReportError puts one up: the message
   beside a sprite for its kind, and the buttons under a divider. Asking whether to remove a
   game or one of its disks, the button that removes it takes a click: Return doesn't press it,
   and Cancel goes back to the settings box as it was left. */
static void draw_report(int win_w, int win_h)
{
	int confirm = dlg == DLG_CONFIRM, r;

	if (confirm && rg_in.key == SDLK_RETURN)
		rg_in.key = 0;
	r = rg_error_box_with((win_w - RG_ERROR_W) / 2, (win_h - RG_ERROR_H + 20) / 2,
			      !confirm && dlg_kind == RG_ERROR ? "Error from PiArcBoot" : "Message from PiArcBoot", dlg_text,
			      confirm ? RG_QUESTION : dlg_kind, confirm ? dlg_disc < 0 && (dlg_game.jaspp || dlg_game.classic) ? "Uninstall" : "Remove" : "OK",
			      confirm ? "Cancel" : NULL);
	if (!confirm)
	{
		if (r || rg_in.key == SDLK_ESCAPE)
			dlg = DLG_NONE;
	}
	else if (r == 2)
		dlg = DLG_SETTINGS;
	else if (r == 1 && dlg_disc >= 0)
	{
		remove_disc(dlg_disc);
		dlg = DLG_SETTINGS;
	}
	else if (r == 1)
	{
		store_remove(&dlg_game);
		icon_forget(dlg_game.id);
		close_settings();
		reload_games();
	}
}

/* A choice of two as a pair of buttons side by side, the one chosen pressed in. Returns 1 when it changes. */
static int toggle(int x, int y, int w, int h, const char *first, const char *second, int *value)
{
	int i, half = w / 2, changed = 0;

	for (i = 0; i < 2; i++)
	{
		int bx = x + i * half, on = *value == i;

		rg_fill(bx, y, half, h, rg_colour(on ? 3 : 1));
		rg_text_centre(RG_DESKTOP, bx, y, half, h, rg_colour(7), i ? second : first);
		rg_border(RG_ACTION, bx, y, half, h, on ? RG_SELECTED : 0);
		if (!on && rg_over(bx, y, half, h) && rg_in.pressed)
		{
			*value = i;
			changed = 1;
		}
	}
	return changed;
}

#define DISK_ROW 30
#define DISK_TABLE_H(n) (24 + (n) * DISK_ROW + 6)

/* The game's disks as a table, a row each from the first down: its image and format, whether the
   game has saved changes to it (kept beside the image, which stays as it was), and whether it
   goes in the drive to be written to or read only. For one of the user's own games with more
   than one disk, each row also has arrows that move it up or down the list and a button that
   removes it. */
static void draw_disks(int x, int y, int w, int mine)
{
	int edit = mine && dlg_game.ndiscs > 1, mode_w = edit ? 160 : 188, i;
	int cols[4] = { x + 8, x + 78, edit ? x + 166 : x + 238, x + w - (edit ? 84 : 8) - mode_w };

	if (!dlg_game.ndiscs)   /* a game that runs from a folder of its own, as Star Fighter 3000 does, has no table, and no room kept for one */
		return;
	rg_text(RG_DESKTOP_BOLD, cols[0], y + 2, rg_colour(7), "Disk");
	rg_text(RG_DESKTOP_BOLD, cols[1], y + 2, rg_colour(7), "Image");
	rg_text(RG_DESKTOP_BOLD, cols[2], y + 2, rg_colour(7), "Changes");
	rg_text(RG_DESKTOP_BOLD, cols[3], y + 2, rg_colour(7), "Mode");
	y += 24;
	rg_slab(x, y, w, dlg_game.ndiscs * DISK_ROW + 6, rg_colour(0), 1);
	for (i = 0; i < dlg_game.ndiscs; i++)
	{
		int ry = y + 3 + i * DISK_ROW, move = 0;
		char text[64];

		if (i)
			rg_fill(x + 3, ry, w - 6, 1, rg_colour(2));
		snprintf(text, sizeof text, "%d", disk_number(&dlg_game, i));
		rg_text(RG_DESKTOP, cols[0] + 8, ry + 5, rg_colour(7), text);
		rg_text_fit(RG_DESKTOP, cols[1], ry + 5, cols[2] - cols[1] - 8, rg_colour(7), dlg_image[i]);
		rg_text(RG_DESKTOP, cols[2], ry + 5, rg_colour(dlg_changed[i] ? RG_RED : 6), dlg_changed[i] ? "Saved" : "None");
		toggle(cols[3], ry + 2, mode_w, DISK_ROW - 4, "Read/write", "Read only", &dlg_ro[i]);
		if (!edit)
			continue;
		/* up takes it nearer the top, as far as the first place, and down nearer the bottom */
		if (rg_bump(cols[0] + 30, ry + 7, &move, i < dlg_game.ndiscs - 1 ? -1 : 0, i ? 1 : 0))
			swap_discs(i, i - move);
		if (rg_button(x + w - 78, ry + 2, 70, DISK_ROW - 4, "Remove", 1))
		{
			snprintf(dlg_text, sizeof dlg_text, "Remove disk %d from %s? Its image, and anything the game saved to it, will be deleted.",
				 i + 1, dlg_game.title);
			dlg_disc = i;
			dlg = DLG_CONFIRM;
		}
	}
}

/* Makes the window tall enough for a box h high, as far as the display it is on has room */
static void grow_window(int h)
{
	SDL_Rect r;
	int ww, wh, wx, wy, top = 0, want = rg_to_window((int)(h * rg_scale + 0.5f));

	if (SDL_GetDisplayUsableBounds(SDL_GetWindowDisplayIndex(rg_win), &r) ||
	    (SDL_GetWindowFlags(rg_win) & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_FULLSCREEN)))
		return;
	SDL_GetWindowSize(rg_win, &ww, &wh);
	SDL_GetWindowPosition(rg_win, &wx, &wy);
	SDL_GetWindowBordersSize(rg_win, &top, NULL, NULL, NULL);
	if (want > r.h - top)
		want = r.h - top;
	if (want <= wh)
		return;
	SDL_SetWindowSize(rg_win, ww, want);
	if (wy + want > r.y + r.h)              /* moved up as far as it has to be to stay on the display */
		SDL_SetWindowPosition(rg_win, wx, r.y + r.h - want);
}

static rg_window_t pane;        /* the settings box's contents, when they are too tall for the window */

/* The dialogue box that is open, as a window over the launcher. Its buttons sit at the bottom
   right with the default one last, and its close icon cancels it. A settings box too tall for
   the window has the window made taller to fit it, as far as the display allows, and what still
   doesn't fit scrolls in a pane above its buttons. */
static void draw_dialog(int win_w, int win_h)
{
	int mine = dlg == DLG_SETTINGS && !dlg_game.jaspp && !dlg_game.classic;
	int thumbs = dlg_nsprites < MAX_THUMBS ? dlg_nsprites : MAX_THUMBS;
	int disks = dlg == DLG_SETTINGS && dlg_game.ndiscs ? DISK_TABLE_H(dlg_game.ndiscs) + 8 : 0;
	int full = (mine ? 324 + (thumbs > 8 ? (thumbs - 1) / 8 * 54 : 0) : dlg == DLG_PROGRESS ? 172 : 160) + disks + (dlg == DLG_SETTINGS ? (mine ? 90 : 150) : 0);
	int h = full < win_h - 8 ? full : win_h - 8;
	int w = DLG_W < win_w - 24 ? DLG_W : win_w - 24, cw = w;
	int x = (win_w - w) / 2, y = (win_h - h) / 2, ok_x, by, cx, cy, i, closed, add = 0, px = rg_in.x, py = rg_in.y;
	char title[200];

	if (dlg == DLG_MESSAGE || dlg == DLG_CONFIRM)
	{
		draw_report(win_w, win_h);
		return;
	}
	if (dlg == DLG_SETTINGS && !dlg_fitted)
	{
		dlg_fitted = 1;
		if (h < full)
			grow_window(full + 8);
	}
	if (y < 4)
		y = 4;
	snprintf(title, sizeof title, "%s", dlg == DLG_SETTINGS ? "" : "Install ");
	snprintf(title + strlen(title), sizeof title - strlen(title), "%s%s", dlg == DLG_SETTINGS ? dlg_game.title : setup_id[0] ? setup_title : dlg_classic ? dlg_classic_entry.title : dlg_entry.title,
		 dlg == DLG_SETTINGS ? " settings" : "");
	closed = rg_dialogue(x, y, w, h, title);
	cx = x + 22;                    /* where the contents start */
	cy = y + 40;
	ok_x = x + w - 26 - BTN_W;
	by = y + h - 22 - BTN_H;

	switch (dlg)
	{
		case DLG_INSTALL:
		rg_option(cx, cy, 22, "Create desktop shortcut", &dlg_desktop, 1);
		rg_option(cx, cy + 30, 22, "Create menu shortcut", &dlg_menu, 1);
		if (rg_button(ok_x - BTN_W - 20, by, BTN_W, BTN_H, "Cancel", 1) || rg_in.key == SDLK_ESCAPE || closed)
			dlg = DLG_NONE;
		else if (default_button(ok_x, by, BTN_W, BTN_H, "Install", 1) || rg_in.key == SDLK_RETURN)
		{
			memset(&progress, 0, sizeof progress);
			progress.percent = -1;
			snprintf(progress.step, sizeof progress.step, "Getting ready...");
			stopping = 0;
			dlg = DLG_PROGRESS;
			SDL_DetachThread(SDL_CreateThread(install_thread, "install", NULL));
		}
		break;

		/* Cancel, Escape and the close icon ask the install to stop, which it does between its
		   steps; the box goes once it has, with nothing to say about it */
		case DLG_PROGRESS:
		if (rg_button(ok_x, by, BTN_W, BTN_H, "Cancel", !stopping) || (!stopping && (rg_in.key == SDLK_ESCAPE || closed)))
		{
			progress.cancel = 1;
			stopping = 1;
		}
		rg_text_fit(RG_DESKTOP, cx, cy + 14, w - 44, rg_colour(7), stopping ? "Stopping the install..." : progress.step);
		rg_slab(cx, cy + 46, w - 44, 18, rg_colour(0), 1);
		if (progress.percent >= 0)
			rg_fill(cx + 2, cy + 48, (w - 48) * progress.percent / 100, 14, rg_colour(RG_DARK_BLUE));
		else                    /* no telling how far along: a block that goes back and forth */
		{
			int span = w - 48 - 80, t = SDL_GetTicks() / 6 % (2 * span);

			rg_fill(cx + 2 + (t < span ? t : 2 * span - t), cy + 48, 80, 14, rg_colour(RG_DARK_BLUE));
		}
		if (progress.done && closing)           /* the window was closed during the install, which has stopped */
			quit = 1;
		else if (progress.done == 1)
		{
			game_t g;

			icon_forget(setup_id[0] ? setup_id : install_id());
			reload_games();
			dlg = DLG_NONE;
			if (!setup_id[0])
				scroll[0] = 0;
			else if (!store_load(setup_id, &g))
				open_settings(&g);      /* as a new game does, to be named and given its icon */
			setup_id[0] = 0;
		}
		else if (progress.done == -1)
		{
			if (stopping)
			{
				reload_games();
				dlg = DLG_NONE;
			}
			else
				message(progress.error, RG_ERROR);
			setup_id[0] = 0;
		}
		break;

		case DLG_SETTINGS:
		if (h < full)
		{
			pane.x = x + 12;
			pane.y = y + 30;
			pane.w = pane.ex = w - 43;
			pane.h = by - 14 - pane.y;
			pane.ey = full - 82;    /* the contents, with 10 pixels above and below */
			pane.sy = dlg_scroll;
			pane.flags = RG_W_VSCROLL;
			rg_window(&pane);
			dlg_scroll = pane.sy;
			cw = w - 19;            /* the contents end as far in from the scroll bar as they do from the box's edge */
			cy = pane.y + 10 - dlg_scroll;
			rg_clip(pane.x, pane.y, pane.w, pane.h);
			if (!rg_over(pane.x, pane.y, pane.w, pane.h))
				rg_in.x = rg_in.y = -1;         /* a pointer outside the pane touches nothing in it */
		}
		if (mine)
		{
			/* labels end where the fields begin, as a RISC OS dialogue box sets them */
			rg_label(cx, cy, 72, 26, "Name", 1);
			rg_field(cx + 80, cy, cw - 124, 26, dlg_name, sizeof dlg_name, 1);
			rg_option(cx + 80, cy + 36, 22, "Create desktop shortcut", &dlg_desktop, 1);
			rg_option(cx + 80 + (cw - 124) / 2, cy + 36, 22, "Create menu shortcut", &dlg_menu, 1);
			rg_label(cx, cy + 68, 72, 26, "Starts by", 1);
			snprintf(title, sizeof title, "%s%s", dlg_choices[dlg_choice][0] ? "running " : "booting the disc", dlg_choices[dlg_choice]);
			rg_display(cx + 80, cy + 68, 230, 26, title);
			if (dlg_nchoices > 1)   /* the arrows step through the applications on its discs */
				rg_bump(cx + 80 + 236, cy + 73, &dlg_choice, 0, dlg_nchoices - 1);
			add = rg_button(x + cw - 22 - 112, cy + 68, 112, BTN_H, "Add disks...", dlg_game.ndiscs < MAX_DISCS);
			if (dlg_nsprites)
				rg_label(cx, cy + 108, 72, 48, "Icon", 1);
			else
				rg_text(RG_DESKTOP, cx + 80, cy + 122, rg_colour(7), "No sprites were found on the discs, so the icon is a plain disc.");
			for (i = 0; i < thumbs; i++)
			{
				int tx = cx + 83 + i % 8 * 54, ty = cy + 108 + i / 8 * 54;

				if (i == dlg_sprite)    /* the one chosen sits in a well */
					rg_slab(tx - 3, ty - 3, 54, 54, rg_colour(0), 1);
				rg_image(dlg_thumbs[i], tx, ty, 48, 48);
				if (rg_over(tx, ty, 48, 48) && rg_in.released)
					dlg_sprite = i;
			}
			rg_option(cx + 80, cy + 108 + (thumbs ? (thumbs - 1) / 8 + 1 : 1) * 54 + 8, 22, "Capture Mouse on Click", &dlg_capture, 1);
			rg_option(cx + 80, cy + 108 + (thumbs ? (thumbs - 1) / 8 + 1 : 1) * 54 + 38, 22, "Slower Mode (A3000 Speed)", &dlg_a3000, 1);
			rg_option(cx + 80, cy + 108 + (thumbs ? (thumbs - 1) / 8 + 1 : 1) * 54 + 68, 22, "Load floppies at a real drive's speed", &dlg_real_drive, 1);
			rg_option(cx + 80, cy + 108 + (thumbs ? (thumbs - 1) / 8 + 1 : 1) * 54 + 98, 22, "Hide Disk Drive Menu", &dlg_hide_drive, 1);
			rg_option(cx + 80, cy + 108 + (thumbs ? (thumbs - 1) / 8 + 1 : 1) * 54 + 128, 22, "Crop to Desktop Window", &dlg_crop, 1);
			draw_disks(cx, cy + 108 + (thumbs ? (thumbs - 1) / 8 + 1 : 1) * 54 + 158, cw - 44, 1);
		}
		else
		{
			rg_option(cx, cy, 22, "Create desktop shortcut", &dlg_desktop, 1);
			rg_option(cx, cy + 30, 22, "Create menu shortcut", &dlg_menu, 1);
			rg_option(cx, cy + 60, 22, "Capture Mouse on Click", &dlg_capture, 1);
			rg_option(cx, cy + 90, 22, "Slower Mode (A3000 Speed)", &dlg_a3000, 1);
			rg_option(cx, cy + 120, 22, "Load floppies at a real drive's speed", &dlg_real_drive, 1);
			rg_option(cx, cy + 150, 22, "Hide Disk Drive Menu", &dlg_hide_drive, 1);
			rg_option(cx, cy + 180, 22, "Crop to Desktop Window", &dlg_crop, 1);
			draw_disks(cx, cy + 216, cw - 44, 0);
		}
		if (h < full)
		{
			rg_unclip();
			rg_in.x = px;
			rg_in.y = py;
		}
		if (add)
		{
			int n;
			char **files = pick_files(0, &n);

			if (n < 0)
			{
				close_settings();
				message("Drop the extra disc images onto the game's settings to add them.", RG_INFORMATION);
			}
			add_files(files, n);
			while (n-- > 0)
				free(files[n]);
			free(files);
			return;
		}
		if (dlg != DLG_SETTINGS)        /* a disk's Remove asks first */
			break;
		if (rg_button(cx, by, BTN_W + 10, BTN_H, mine ? "Remove" : "Uninstall", 1))
		{
			snprintf(dlg_text, sizeof dlg_text, "%s %s? Its disc images, anything it saved to them and its save states will be deleted.",
				 mine ? "Remove" : "Uninstall", dlg_game.title);
			dlg_disc = -1;
			dlg = DLG_CONFIRM;
		}
		else if (rg_button(ok_x - BTN_W - 20, by, BTN_W, BTN_H, "Cancel", 1) || rg_in.key == SDLK_ESCAPE || closed)
			close_settings();
		else if (default_button(ok_x, by, BTN_W, BTN_H, "OK", 1) || rg_in.key == SDLK_RETURN)
		{
			game_t g;

			if (!store_load(dlg_game.id, &g))
			{
				g.desktop = dlg_desktop;
				g.menu = dlg_menu;
				g.nocapture = !dlg_capture;
				g.a3000 = dlg_a3000;
				g.real_drive = dlg_real_drive;
				g.hide_drive = dlg_hide_drive == (g.ndiscs <= 1) ? -1 : dlg_hide_drive;    /* the default goes on following the number of discs */
				g.crop = dlg_crop == preset_crop(&g) ? -1 : dlg_crop;    /* and the presets' */
				if (g.ndiscs == dlg_game.ndiscs)        /* the disks in the order the box has them */
				{
					for (i = 0; i < g.ndiscs; i++)
						if (strcmp(g.disc[i], dlg_game.disc[i]))
						{
							forget_swaps(g.id);
							break;
						}
					memcpy(g.disc, dlg_game.disc, sizeof g.disc);
					for (i = 0; i < g.ndiscs; i++)
						g.ro[i] = (uint8_t)dlg_ro[i];
				}
				if (mine)
				{
					if (dlg_name[0])
						snprintf(g.title, sizeof g.title, "%s", dlg_name);
					snprintf(g.boot, sizeof g.boot, "%s", dlg_choices[dlg_choice]);
					if (dlg_sprite >= 0 && strcmp(g.sprite, dlg_sprites[dlg_sprite].name))
					{
						snprintf(g.sprite, sizeof g.sprite, "%s", dlg_sprites[dlg_sprite].name);
						store_set_icon(&g, &dlg_sprites[dlg_sprite]);
					}
				}
				icon_forget(g.id);
				store_save(&g);
				store_shortcuts(&g);
			}
			close_settings();
			reload_games();
		}
		break;
	}
}

/* The tabs along the top of the pane, each outlined in black as the pane is. The one in front
   is the pane's own grey and opens into it; the other sits behind it, darker and lower. */
static void draw_tabs(void)
{
	int k;

	for (k = 0; k <= TABS; k++)     /* the ones behind first, then the one in front over their edges */
	{
		int front = k == TABS, i = front ? tab : k, x = 10 + i * (TAB_W - 1), y = front ? TABS_Y : TABS_Y + 4, h = TABS_Y + 32 - y;
		int font = front ? RG_DESKTOP_BOLD : RG_DESKTOP, nw, cw, left;
		char count[16] = "";

		if (!front && i == tab)
			continue;
		rg_rect(x, y, TAB_W, h, rg_colour(7));
		if (front)
		{
			rg_tile_origin(0, 0);
			rg_marble(x + 1, y + 1, TAB_W - 2, h - 1);
		}
		else
			rg_fill(x + 1, y + 1, TAB_W - 2, h - 2, rg_colour(2));
		if (search[0] && i != TAB_ABOUT)        /* how many of its games match, in red after its name */
			snprintf(count, sizeof count, " (%d)", tab_count(i));
		nw = rg_text_w(font, tab_names[i]);
		cw = rg_text_w(font, count);
		left = x + (TAB_W - nw - cw) / 2;
		rg_text_centre(font, left, y, nw, front ? 30 : 27, rg_colour(7), tab_names[i]);
		if (count[0])
			rg_text_centre(font, left + nw, y, cw, front ? 30 : 27, rg_colour(RG_RED), count);
	}
}

/* The search field over the tabs. It has the caret unless the field for a new PackMan source has. */
static void draw_search(int w)
{
	int lw = rg_text_w(RG_DESKTOP, "Search:"), fw = w - 34 - lw, cleared = 0;

	if (fw > 280)
		fw = 280;
	rg_text_centre(RG_DESKTOP, 12, 10, lw, 28, rg_colour(7), "Search:");
	if (rg_in.pressed)
		source_focus = 0;               /* and the Classics tab gives it back for a click in its field */
	if (!source_focus && rg_in.key == SDLK_ESCAPE && search[0])
	{
		search[0] = 0;
		cleared = 1;
	}
	if (rg_field(20 + lw, 10, fw, 28, search, sizeof search, !source_focus) || cleared)
	{
		memset(scroll, 0, sizeof scroll);       /* each list from its top for what it now shows, with nothing highlighted */
		memset(sel, -1, sizeof sel);
	}

	/* PiArcBoot's name and logo in the corner the field leaves */
	{
		int font = rg_font(RG_BOLD, 14 * 16), tw = rg_text_w(font, "PiArcBoot"), lx = w - 12 - 34;

		if (lx - 10 - tw > 20 + lw + fw + 12)
		{
			rg_image(app_tex, lx, 6, 34, 34);
			rg_text_centre(font, lx - 8 - tw, 6, tw, 34, rg_colour(7), "PiArcBoot");
		}
	}
}

/* The window was closed. An install that is going is stopped first, and the window stays until it
   has; closing it again meanwhile quits at once. */
static void close_window(void)
{
	if (dlg == DLG_PROGRESS && !progress.done && !closing)
	{
		progress.cancel = 1;
		stopping = closing = 1;
	}
	else
		quit = 1;
}

int launcher_main(const char *test_script)
{
	uint8_t *rgba = malloc(128 * 128 * 4);
	char **dropped = NULL;
	int n_dropped = 0, frame = 0, drawn = 0, hand_shown = 0;
	SDL_Cursor *hand = NULL;     /* a link's pointer, as a web page's is */

	icon_app(rgba);
	if (rg_open("PiArcBoot", 660, 500, RG_RESIZABLE, "piarcboot", rgba))
	{
		fprintf(stderr, "piarcboot: %s%c", SDL_GetError(), 10);
		return 1;
	}
	SDL_SetWindowMinimumSize(rg_win, rg_to_window((int)(600 * rg_scale)), rg_to_window((int)(400 * rg_scale)));
	SDL_EventState(SDL_DROPFILE, SDL_ENABLE);
	SDL_StartTextInput();
	app_tex = rg_image_texture(rgba, 128, 128, 1);
	icon_generic(rgba);
	generic_tex = rg_image_texture(rgba, 128, 128, 1);
	free(rgba);

	reload_games();
	load_feed();
	if (test_script)
		rg_test_load(test_script);
	else
		SDL_DetachThread(SDL_CreateThread(feed_thread, "feed", NULL));

	while (!quit)
	{
		const char *shot = NULL, *drop = NULL;
		char text[sizeof rg_in.text];
		int w, h, px, py, pressed, released, wheel, key, i, was_open;
		SDL_Event e;

		rg_begin();
		/* Nothing is drawn while nothing happens: the window waits for an event, and looks up
		   in between only for what changes by itself (a download, the feed arriving, a scroll
		   arrow held down) */
		if (!test_script && !SDL_WaitEventTimeout(NULL, !drawn ? 1 : dlg == DLG_PROGRESS || rg_in.down ? 16 : 250) &&
		    dlg == DLG_NONE && !feed_arrived && drawn && !rg_in.down)
			continue;
		drawn = 1;
		while (SDL_PollEvent(&e))
		{
			rg_event(&e);
			if (e.type == SDL_QUIT)
				close_window();
			else if (e.type == SDL_DROPFILE)
			{
				dropped = realloc(dropped, (n_dropped + 1) * sizeof *dropped);
				dropped[n_dropped++] = strdup(e.drop.file);
				SDL_free(e.drop.file);
			}
		}
		if (test_script)
		{
			if (rg_test_step(frame, &shot, &drop))
				close_window();         /* as closing the window does */
			if (drop)
			{
				dropped = realloc(dropped, (n_dropped + 1) * sizeof *dropped);
				dropped[n_dropped++] = strdup(drop);
			}
		}
		frame++;
		if (n_dropped)
		{
			add_files(dropped, n_dropped);
			while (n_dropped > 0)
				free(dropped[--n_dropped]);
		}
		if (feed_arrived)
		{
			feed_arrived = 0;
			load_feed();
		}

		rg_size(&w, &h);
		rg_tile_origin(0, 0);
		rg_marble(0, 0, w, h);
		over_link = 0;

		/* With a dialogue box open the window behind it is drawn but takes no input */
		px = rg_in.x; py = rg_in.y; pressed = rg_in.pressed; released = rg_in.released; wheel = rg_in.wheel; key = rg_in.key;
		was_open = dlg != DLG_NONE;
		memcpy(text, rg_in.text, sizeof text);
		if (was_open)
		{
			rg_in.x = rg_in.y = -1;
			rg_in.pressed = rg_in.released = rg_in.wheel = rg_in.key = 0;
			rg_in.text[0] = 0;
		}
		for (i = 0; i < TABS; i++)
			if (rg_in.pressed && rg_over(10 + i * (TAB_W - 1), TABS_Y, TAB_W, 31))
				tab = i;
		if (rg_in.key == SDLK_TAB && (SDL_GetModState() & KMOD_CTRL))  /* Ctrl+Tab to the next tab, and with Shift the one before */
			tab = (tab + (SDL_GetModState() & KMOD_SHIFT ? TABS - 1 : 1)) % TABS;
		draw_search(w);
		if (draw_list(11, TABS_Y + 32, w - 22 - BAR_W + 1, h - TABS_Y - 43))
			drawn = 0;              /* once more, with the scroll bar the length the list makes it */
		draw_tabs();
		rg_in.x = px; rg_in.y = py; rg_in.pressed = pressed; rg_in.released = released; rg_in.wheel = wheel; rg_in.key = key;
		memcpy(rg_in.text, text, sizeof text);
		if (!was_open)          /* the click that opened a dialogue box doesn't press what is under it in the box */
			rg_in.pressed = rg_in.released = rg_in.key = 0;

		if (dlg != DLG_NONE)
		{
			draw_dialog(w, h);
			if (dlg == DLG_NONE)    /* it closed, by itself or by a click: the window is drawn once more without it */
				drawn = 0;
		}
		if (over_link != hand_shown)
		{
			if (!hand)
				hand = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_HAND);
			SDL_SetCursor(over_link && hand ? hand : SDL_GetDefaultCursor());
			hand_shown = over_link;
		}
		if (shot)
			rg_save_shot(shot);
		rg_present();
	}
	SDL_Quit();
	return 0;
}
