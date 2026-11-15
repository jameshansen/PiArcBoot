/* Where games live and how they get there.

   Every game has a folder of its own under the data folder (~/.local/share/piarcboot/games on
   Linux, %APPDATA%\PiArcBoot\games on Windows) holding its disc images, its icon and game.ini,
   its settings. A JASPP game is fetched into one: the package from JASPP's PackMan feed where it
   holds every floppy image of the game, and otherwise the floppy images from JASPP's release
   folder for the game. A game of the user's own is copied into one from the disc images given.
   Shortcuts on the desktop and in the menu point back at "piarcboot play <game>". */
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#else
#include <sys/wait.h>
#endif
#include "piarcboot.h"
#include "catalog.h"

#define FEED_URL "http://www.jaspp.org.uk/packages/release"
#define PACKAGE_HOST "http://www.jaspp.org.uk"
#define RELEASE_URL "https://forums.jaspp.org.uk/release/F%05u/Discs/"

static int exists(const char *path)
{
	struct stat st;

	return !stat(path, &st);
}

const char *store_dir(void)
{
	static char dir[700];

	if (!dir[0])
	{
		const char *home = getenv("PIARCBOOT_HOME");
		char games[720];

		if (home)
			snprintf(dir, sizeof dir, "%s", home);
#ifdef _WIN32
		else
			snprintf(dir, sizeof dir, "%s/PiArcBoot", getenv("APPDATA") ? getenv("APPDATA") : ".");
#else
		else if (getenv("XDG_DATA_HOME"))
			snprintf(dir, sizeof dir, "%s/piarcboot", getenv("XDG_DATA_HOME"));
		else
			snprintf(dir, sizeof dir, "%s/.local/share/piarcboot", getenv("HOME") ? getenv("HOME") : ".");
#endif
		snprintf(games, sizeof games, "%s/games", dir);
		make_dirs(games);
	}
	return dir;
}

/* The full path of this program */
const char *store_exe(void)
{
	static char exe[700];

	if (!exe[0])
	{
#ifdef _WIN32
		GetModuleFileNameA(NULL, exe, sizeof exe - 1);
#else
		ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);

		if (n < 0)
			snprintf(exe, sizeof exe, "piarcboot");
#endif
	}
	return exe;
}

/* A file that comes with the program (the ROM, the game icons): beside the program, one folder
   up (where a build leaves it), or in ../share/piarcboot. Returns buf, or NULL if it isn't there. */
char *store_asset(const char *name, char *buf, size_t size)
{
	static const char *const where[] = { "", "../", "../share/piarcboot/" };
	char dir[700];
	size_t i;

	snprintf(dir, sizeof dir, "%s", store_exe());
	dir[base_name(dir) - dir] = 0;
	for (i = 0; i < sizeof where / sizeof *where; i++)
	{
		snprintf(buf, size, "%s%s%s", dir, where[i], name);
		if (exists(buf))
			return buf;
	}
	return NULL;
}

void store_game_path(const char *id, const char *file, char *buf, size_t size)
{
	snprintf(buf, size, "%s/games/%s%s%s", store_dir(), id, file[0] ? "/" : "", file);
}

int store_load(const char *id, game_t *g)
{
	char path[1024], *text, *line, *next, ro[MAX_DISCS][40];
	int n_ro = 0, all_ro = 0, i, k;

	store_game_path(id, "game.ini", path, sizeof path);
	text = (char *)file_read(path, NULL);
	if (!text)
		return -1;
	memset(g, 0, sizeof *g);
	snprintf(g->id, sizeof g->id, "%s", id);
	g->scale = 2;
	g->crop = -1;
	g->autosave = 1;
	for (line = text; line && *line; line = next)
	{
		char *eq, *v;

		next = strchr(line, '\n');
		if (next)
			*next++ = 0;
		line[strcspn(line, "\r")] = 0;
		eq = strchr(line, '=');
		if (!eq)
			continue;
		*eq = 0;
		v = eq + 1;
		if (!strcmp(line, "title")) snprintf(g->title, sizeof g->title, "%s", v);
		else if (!strcmp(line, "info")) snprintf(g->info, sizeof g->info, "%s", v);
		else if (!strcmp(line, "boot")) snprintf(g->boot, sizeof g->boot, "%s", v);
		else if (!strcmp(line, "version")) snprintf(g->version, sizeof g->version, "%s", v);
		else if (!strcmp(line, "sprite")) snprintf(g->sprite, sizeof g->sprite, "%s", v);
		else if (!strcmp(line, "preset")) snprintf(g->preset, sizeof g->preset, "%s", v);
		else if (!strcmp(line, "crop")) g->crop = atoi(v);
		else if (!strcmp(line, "autosave")) g->autosave = atoi(v) >= 0 && atoi(v) <= 3 ? atoi(v) : 1;
		else if (!strcmp(line, "fdc")) g->fdc = atoi(v);
		else if (!strcmp(line, "wp")) all_ro = atoi(v);         /* every disc write protected, as the JASPP table has some */
		else if (!strcmp(line, "readonly") && n_ro < MAX_DISCS) snprintf(ro[n_ro++], sizeof ro[0], "%s", v);
		else if (!strcmp(line, "jaspp")) g->jaspp = atoi(v);
		else if (!strcmp(line, "classic")) g->classic = atoi(v);
		else if (!strcmp(line, "apps")) g->apps = atoi(v);
		else if (!strcmp(line, "scale")) g->scale = atoi(v) < 1 ? 1 : atoi(v) > 3 ? 3 : atoi(v);
		else if (!strcmp(line, "fullscreen")) g->fullscreen = atoi(v);
		else if (!strcmp(line, "capture")) g->nocapture = !atoi(v);
		else if (!strcmp(line, "desktop")) g->desktop = atoi(v);
		else if (!strcmp(line, "menu")) g->menu = atoi(v);
		else if (!strcmp(line, "disc") && g->ndiscs < MAX_DISCS && !strpbrk(v, "/\\"))
			snprintf(g->disc[g->ndiscs++], sizeof g->disc[0], "%s", v);
	}
	free(text);
	for (i = 0; i < g->ndiscs; i++)
		for (g->ro[i] = (uint8_t)all_ro, k = 0; k < n_ro; k++)
			g->ro[i] |= !strcmp(ro[k], g->disc[i]);
	return 0;
}

/* Whether a game has written to that disc: what it wrote is kept in a .sav file beside the image */
int store_disc_changed(const game_t *g, int n)
{
	char path[1100];

	store_game_path(g->id, g->disc[n], path, sizeof path);
	snprintf(path + strlen(path), sizeof path - strlen(path), ".sav");
	return exists(path);
}

int store_save(const game_t *g)
{
	char path[1024];
	FILE *f;
	int i;

	store_game_path(g->id, "", path, sizeof path);
	make_dirs(path);
	store_game_path(g->id, "game.ini", path, sizeof path);
	f = fopen(path, "w");
	if (!f)
		return -1;
	fprintf(f, "title=%s\ninfo=%s\nboot=%s\nversion=%s\nsprite=%s\nfdc=%d\njaspp=%d\nclassic=%d\napps=%d\nscale=%d\nfullscreen=%d\ncapture=%d\ndesktop=%d\nmenu=%d\n",
		g->title, g->info, g->boot, g->version, g->sprite, g->fdc, g->jaspp, g->classic, g->apps, g->scale, g->fullscreen, !g->nocapture,
		g->desktop, g->menu);
	if (g->preset[0])
		fprintf(f, "preset=%s\n", g->preset);
	if (g->crop >= 0)
		fprintf(f, "crop=%d\n", g->crop);
	if (g->autosave != 1)
		fprintf(f, "autosave=%d\n", g->autosave);
	for (i = 0; i < g->ndiscs; i++)
		fprintf(f, "disc=%s\n", g->disc[i]);
	for (i = 0; i < g->ndiscs; i++)
		if (g->ro[i])
			fprintf(f, "readonly=%s\n", g->disc[i]);
	return fclose(f) ? -1 : 0;
}

static int by_title(const void *a, const void *b)
{
	const char *s = ((const game_t *)a)->title, *t = ((const game_t *)b)->title;

	for (; *s && tolower((unsigned char)*s) == tolower((unsigned char)*t); s++, t++);
	return tolower((unsigned char)*s) - tolower((unsigned char)*t);
}

/* Every installed game, in order of title. Free *out. */
int store_list(game_t **out)
{
	char path[1024];
	struct dirent *e;
	game_t *list = NULL;
	int n = 0;
	DIR *d;

	snprintf(path, sizeof path, "%s/games", store_dir());
	d = opendir(path);
	while (d && (e = readdir(d)))
	{
		game_t g;

		if (e->d_name[0] == '.' || strlen(e->d_name) >= sizeof g.id || store_load(e->d_name, &g))
			continue;
		list = realloc(list, (n + 1) * sizeof *list);
		list[n++] = g;
	}
	if (d)
		closedir(d);
	if (n)
		qsort(list, n, sizeof *list, by_title);
	*out = list;
	return n;
}

#ifndef _WIN32
/* The name a game's window goes by on the desktop, which its menu entry carries too so that
   the taskbar shows the game's icon */
static void window_class(const char *id, char *buf, size_t size)
{
	size_t i;

	snprintf(buf, size, "piarcboot-%s", id);
	for (i = 0; buf[i]; i++)
		if (!isalnum((unsigned char)buf[i]) && buf[i] != '-' && buf[i] != '_')
			buf[i] = '_';
}

#endif

#ifdef _WIN32
/* A shortcut on the desktop (kind "desktop") or in the Start menu ("menu"), named after the game.
   A file in the game's folder records that PiArcBoot made it, and only a shortcut with that
   record is ever deleted, so a shortcut of the user's own with the same name is safe. With
   PIARCBOOT_HOME set, which the tests do, shortcuts are kept inside that folder. */
static void shortcut(const char *folder_var, const char *sub, const char *kind, const game_t *g, int wanted)
{
	char lnk[1024], ps1[1024], icon[1024], made[1024], dir[900], title[128];
	size_t i;
	FILE *f;

	snprintf(title, sizeof title, "%s", g->title);
	for (i = 0; title[i]; i++)
		if (strchr("\\/:*?\"<>|'", title[i]))
			title[i] = ' ';
	if (getenv("PIARCBOOT_HOME"))
		snprintf(dir, sizeof dir, "%s\\%s", getenv("PIARCBOOT_HOME"), kind);
	else
		snprintf(dir, sizeof dir, "%s%s", getenv(folder_var) ? getenv(folder_var) : ".", sub);
	make_dirs(dir);
	snprintf(lnk, sizeof lnk, "%s\\%s.lnk", dir, title);
	snprintf(ps1, sizeof ps1, "%s.made", kind);
	store_game_path(g->id, ps1, made, sizeof made);
	if (!wanted)
	{
		if (!remove(made))
			DeleteFileA(lnk);
		return;
	}
	file_write(made, "", 0);
	store_game_path(g->id, "icon.ico", icon, sizeof icon);
	store_game_path(g->id, "shortcut.ps1", ps1, sizeof ps1);
	f = fopen(ps1, "w");
	if (!f)
		return;
	fprintf(f, "$s = (New-Object -ComObject WScript.Shell).CreateShortcut('%s')\n$s.TargetPath = '%s'\n"
		   "$s.Arguments = 'play \"%s\"'\n$s.IconLocation = '%s'\n$s.Save()\n", lnk, store_exe(), g->id, icon);
	fclose(f);
	run_command("powershell -NoProfile -ExecutionPolicy Bypass -File \"%s\"", ps1);
	DeleteFileA(ps1);
}
#endif

#ifndef _WIN32
/* The folders menu entries and desktop shortcuts go in. With PIARCBOOT_HOME set, which the tests
   do, both are kept inside that folder and the real desktop and menu are left alone. */
static void shortcut_dirs(char *apps, size_t apps_size, char *desk, size_t desk_size)
{
	const char *test = getenv("PIARCBOOT_HOME"), *data = getenv("XDG_DATA_HOME");
	const char *home = getenv("HOME") ? getenv("HOME") : ".";
	char *dir;

	if (test)
	{
		snprintf(apps, apps_size, "%s/applications", test);
		snprintf(desk, desk_size, "%s/desktop", test);
		make_dirs(desk);
		return;
	}
	if (data)
		snprintf(apps, apps_size, "%s/applications", data);
	else
		snprintf(apps, apps_size, "%s/.local/share/applications", home);
	dir = run_output("xdg-user-dir DESKTOP 2>/dev/null");
	if (dir)
		dir[strcspn(dir, "\r\n")] = 0;
	if (dir && dir[0] == '/')
		snprintf(desk, desk_size, "%s", dir);
	else
		snprintf(desk, desk_size, "%s/Desktop", home);
	free(dir);
}
#endif

/* Where Save Snapshot puts its pictures: the user's Pictures folder, or with PIARCBOOT_HOME set,
   which the tests do, a folder inside that */
void store_pictures_dir(char *buf, size_t size)
{
	const char *test = getenv("PIARCBOOT_HOME");
#ifdef _WIN32
	char dir[MAX_PATH];
#else
	const char *home = getenv("HOME") ? getenv("HOME") : ".";
	char *dir;
#endif

	if (test)
		snprintf(buf, size, "%s/snapshots", test);
	else
	{
#ifdef _WIN32
		if (SHGetFolderPathA(NULL, CSIDL_MYPICTURES, NULL, 0, dir) == S_OK)  /* wherever the Pictures folder has been moved to */
			snprintf(buf, size, "%s", dir);
		else
			snprintf(buf, size, "%s\\Pictures", getenv("USERPROFILE") ? getenv("USERPROFILE") : ".");
#else
		dir = run_output("xdg-user-dir PICTURES 2>/dev/null");
		if (dir)
			dir[strcspn(dir, "\r\n")] = 0;
		if (dir && dir[0] == '/')
			snprintf(buf, size, "%s", dir);
		else
			snprintf(buf, size, "%s/Pictures", home);
		free(dir);
#endif
	}
	make_dirs(buf);
}

/* Makes a game's shortcuts match its settings: one on the desktop, one in the menu */
void store_shortcuts(const game_t *g)
{
#ifdef _WIN32
	shortcut("USERPROFILE", "\\Desktop", "desktop", g, g->desktop);
	shortcut("APPDATA", "\\Microsoft\\Windows\\Start Menu\\Programs", "menu", g, g->menu);
#else
	char cls[160], apps[900], deskdir[900], menu[1100], desk[1100], icon[1024];
	FILE *f;
	int i;

	window_class(g->id, cls, sizeof cls);
	shortcut_dirs(apps, sizeof apps, deskdir, sizeof deskdir);
	make_dirs(apps);
	snprintf(menu, sizeof menu, "%s/%s.desktop", apps, cls);
	snprintf(desk, sizeof desk, "%s/%s.desktop", deskdir, cls);
	store_game_path(g->id, "icon.png", icon, sizeof icon);

	/* The menu entry is always there, hidden when it isn't wanted, since the taskbar looks a
	   window's icon up through it */
	for (i = 0; i < 2; i++)
	{
		const char *path = i ? desk : menu;

		if (i && !g->desktop)
		{
			unlink(path);
			continue;
		}
		f = fopen(path, "w");
		if (!f)
			continue;
		fprintf(f, "[Desktop Entry]\nType=Application\nName=%s\nComment=An Acorn Archimedes game, started by PiArcBoot\n"
			   "Exec=\"%s\" play \"%s\"\nIcon=%s\nTerminal=false\nCategories=Game;\nStartupWMClass=%s\n%s",
			g->title, store_exe(), g->id, icon, cls, !i && !g->menu ? "NoDisplay=true\n" : "");
		fclose(f);
		chmod(path, 0755);
	}
#endif
}

/* Takes a game off the computer: its shortcuts, then its folder */
void store_remove(const game_t *g)
{
	game_t none = *g;
	char path[1024];

	none.desktop = none.menu = 0;
	store_shortcuts(&none);
#ifndef _WIN32
	{
		char cls[160], apps[900], desk[900], entry[1100];

		window_class(g->id, cls, sizeof cls);
		shortcut_dirs(apps, sizeof apps, desk, sizeof desk);
		snprintf(entry, sizeof entry, "%s/%s.desktop", apps, cls);
		unlink(entry);
	}
#endif
	store_game_path(g->id, "", path, sizeof path);
	remove_tree(path);
}

/* A game's icon, 128 by 128 RGBA: its own, or the plain floppy disc */
void store_icon(const game_t *g, uint8_t *out)
{
	char path[1024];
	uint8_t *px;
	int w, h;

	store_game_path(g->id, "icon.png", path, sizeof path);
	px = png_read(path, &w, &h);
	if (px && w == 128 && h == 128)
		memcpy(out, px, 128 * 128 * 4);
	else
		icon_generic(out);
	free(px);
}

/* Writes an icon into a game's folder as icon.png, and as icon.ico for Windows shortcuts (an
   .ico file can hold a PNG as it is) */
static int write_icon(const char *id, const uint8_t *rgba)
{
	char path[1024];
	int r;

	store_game_path(id, "icon.png", path, sizeof path);
	r = png_write(path, rgba, 128, 128);
#ifdef _WIN32
	{
		static const uint8_t head[22] = { 0, 0, 1, 0, 1, 0, 128, 128, 0, 0, 1, 0, 32, 0, 0, 0, 0, 0, 22, 0, 0, 0 };
		uint8_t *png, *ico;
		size_t n;

		png = file_read(path, &n);
		if (png)
		{
			ico = malloc(n + 22);
			memcpy(ico, head, 22);
			ico[14] = n; ico[15] = n >> 8; ico[16] = n >> 16; ico[17] = n >> 24;
			memcpy(ico + 22, png, n);
			store_game_path(id, "icon.ico", path, sizeof path);
			file_write(path, ico, n + 22);
			free(ico);
			free(png);
		}
	}
#endif
	return r;
}

/* A disc image as plain sectors, for looking through its files. A bit level image (HFE) comes
   back as it is, in which nothing will be found. */
static uint8_t *image_sectors(const char *path, size_t *len)
{
	const char *dot = strrchr(path, '.');

	if (dot && (!strcmp(dot, ".jfd") || !strcmp(dot, ".JFD")))
		return jfd_flatten(path, len);
	return file_read(path, len);
}

/* The sprites on a game's discs, to pick its icon from */
int store_sprites(const game_t *g, sprite_t **out)
{
	sprite_t *all = NULL;
	int total = 0, i, j;

	/* a game in a HostFS folder has no discs, and its application's sprite file is in the folder */
	for (i = g->ndiscs ? 0 : -1; i < g->ndiscs; i++)
	{
		char path[1024], file[200];
		sprite_t *list;
		uint8_t *img;
		size_t len;
		int n;

		if (i < 0)
		{
			snprintf(file, sizeof file, "hostfs/%s/!Sprites,ff9", g->boot);
			store_game_path(g->id, file, path, sizeof path);
			img = file_read(path, &len);
		}
		else
		{
			store_game_path(g->id, g->disc[i], path, sizeof path);
			img = image_sectors(path, &len);
		}
		if (!img)
			continue;
		n = sprites_scan(img, len, &list);
		free(img);
		for (j = 0; j < n; j++)
		{
			all = realloc(all, (total + 1) * sizeof *all);
			all[total++] = list[j];
		}
		free(list);
	}
	*out = all;
	return total;
}

int store_set_icon(const game_t *g, const sprite_t *s)
{
	uint8_t *rgba = malloc(128 * 128 * 4);
	int r;

	if (s)
		icon_render(rgba, s);
	else
		icon_generic(rgba);
	r = write_icon(g->id, rgba);
	free(rgba);
	return r;
}

/* Names as RISC OS sorts them, without regard to case */
static int by_choice(const void *a, const void *b)
{
	const char *x = a, *y = b;

	for (; *x && tolower((unsigned char)*x) == tolower((unsigned char)*y); x++, y++);
	return tolower((unsigned char)*x) - tolower((unsigned char)*y);
}

static int is_system(const char *name)
{
	static const char *const sys[] = { "!boot", "!system", "!fonts", "!scrap", "!sysmerge", "!fontmerge", "!help" };
	char low[64];
	size_t i;

	for (i = 0; name[i] && i < sizeof low - 1; i++)
		low[i] = tolower((unsigned char)name[i]);
	low[i] = 0;
	for (i = 0; i < sizeof sys / sizeof *sys; i++)
		if (!strcmp(low, sys[i]))
			return 1;
	return 0;
}

/* The ways a game's first disc can be started: "" (boot the disc), then each application in its
   root directory. The one the disc itself points to comes first: booting, if the disc has a boot
   option set, and otherwise its first application. Returns how many. */
int store_boot_choices(const game_t *g, char choices[][64], int max)
{
	char path[1024], names[80][64], name[64];
	int n, i, k, count = 0, option = 0, first_app = -1;
	adfs_file_t *files;
	uint8_t *img;
	size_t len;

	if (!g->ndiscs)         /* a game in a HostFS folder: the applications in it, the first that isn't the system's first */
	{
		DIR *d;
		struct dirent *e;

		store_game_path(g->id, "hostfs", path, sizeof path);
		d = opendir(path);
		while (d && (e = readdir(d)) && count < max && count < 80)
		{
			char sub[1100];

			snprintf(sub, sizeof sub, "%s/%s", path, e->d_name);
			if (e->d_name[0] == '!' && strlen(e->d_name) < 64 && is_dir(sub))
				snprintf(names[count++], 64, "%s", e->d_name);
		}
		if (d)
			closedir(d);
		qsort(names, (size_t)count, sizeof names[0], by_choice);
		for (i = 0, n = 0; i < count; i++)
			if (!is_system(names[i]))
				snprintf(choices[n++], 64, "%s", names[i]);
		for (i = 0; i < count; i++)
			if (is_system(names[i]))
				snprintf(choices[n++], 64, "%s", names[i]);
		return n;
	}
	/* the names in the root of the first disc, a hard space in one as the # that matches it on a
	   command line */
	snprintf(choices[count++], 64, "%s", "");
	store_game_path(g->id, g->disc[0], path, sizeof path);
	img = disc_sectors(path, &len);
	n = img ? adfs_read(img, len, &files, NULL, &option) : -1;
	free(img);
	for (i = 0; i < n && count < max; i++)
		if (files[i].path[0] == '!' && !strchr(files[i].path, '.') && strlen(files[i].path) < 64)
		{
			for (k = 0; files[i].path[k]; k++)
				name[k] = (unsigned char)files[i].path[k] == 0xA0 ? '#' : files[i].path[k];
			name[k] = 0;
			if (first_app < 0 && !is_system(name))
				first_app = count;
			snprintf(choices[count++], 64, "%s", name);
		}
	adfs_free(files, n);
	if (n >= 0 && !option && first_app > 0)        /* not a booting disc: lead with its application */
	{
		char t[64];

		memcpy(t, choices[first_app], 64);
		memmove(choices[1], choices[0], (size_t)first_app * 64);
		memcpy(choices[0], t, 64);
	}
	return count;
}

/* Whether a disc image is a 1.6MB high density disc, which needs the later floppy controller */
static int image_is_hd(const char *path)
{
	size_t len;
	uint8_t *d = jfd_read(path, &len);
	int hd = 0;

	if (!d)
		return 0;
	if (len > 316 && !memcmp(d, "JFDI", 4))
	{
		uint32_t at = d[28] | d[29] << 8 | d[30] << 16 | (uint32_t)d[31] << 24, track0;

		track0 = d[24] | d[25] << 8 | d[26] << 16 | (uint32_t)d[27] << 24;
		if (track0 + 4 <= len)          /* the first sector of the first track */
		{
			uint32_t t = d[track0] | d[track0 + 1] << 8 | d[track0 + 2] << 16 | (uint32_t)d[track0 + 3] << 24;

			if (t != 0xffffffff && (size_t)at + t + 4 <= len)
				hd = (d[at + t + 2] & 15) == 4;
		}
	}
	else if (len > 20 && !memcmp(d, "HXCPICFE", 8))
		hd = (d[12] | d[13] << 8) >= 400;
	else
		hd = len > 1200 * 1024;
	free(d);
	return hd;
}

/* The JASPP list: the built-in table, brought up to date by the feed */
static entry_t *entries;
static int n_entries;

static void catalog_init(void)
{
	size_t i;

	if (entries)
		return;
	entries = calloc(sizeof catalog / sizeof *catalog + 64, sizeof *entries);
	for (i = 0; i < sizeof catalog / sizeof *catalog; i++)
	{
		entry_t *e = &entries[n_entries++];

		snprintf(e->id, sizeof e->id, "%s", catalog[i].id);
		snprintf(e->title, sizeof e->title, "%s", catalog[i].title);
		snprintf(e->info, sizeof e->info, "%s", catalog[i].info);
		snprintf(e->boot, sizeof e->boot, "%s", catalog[i].boot);
		snprintf(e->version, sizeof e->version, "%s", catalog[i].version);
		e->game = catalog[i].game;
		e->size = catalog[i].size;
		e->fdc = catalog[i].fdc;
		e->wp = catalog[i].wp;
		e->discs = catalog[i].discs;
		e->set = catalog[i].set;
		e->release = catalog[i].release;
		e->floppies = e->discs > e->release ? e->discs : e->release;
		e->tested = 1;
	}
}

int catalog_count(void)
{
	catalog_init();
	return n_entries;
}

const entry_t *catalog_entry(int i)
{
	catalog_init();
	return &entries[i];
}

static const char *field(const char *block, const char *end, const char *name, char *buf, size_t size)
{
	size_t n = strlen(name);
	const char *p;

	buf[0] = 0;
	for (p = block; p < end; p = memchr(p, '\n', end - p) ? (const char *)memchr(p, '\n', end - p) + 1 : end)
		if (!strncmp(p, name, n) && p[n] == ':')
		{
			const char *v = p + n + 1, *eol = memchr(v, '\n', end - v);

			while (*v == ' ')
				v++;
			snprintf(buf, size, "%.*s", (int)((eol ? eol : end) - v), v);
			buf[strcspn(buf, "\r")] = 0;
			break;
		}
	return buf;
}

/* How many floppy images a game that isn't in the table has in its release folder, as noted in
   floppies.txt in the data folder by feed_check_new, or -1 if it hasn't been looked at yet */
static int noted_floppies(const char *name)
{
	char path[1024], *text, *line;
	int count = -1;

	snprintf(path, sizeof path, "%s/floppies.txt", store_dir());
	text = (char *)file_read(path, NULL);
	for (line = text; line && *line; line = strchr(line, 10) ? strchr(line, 10) + 1 : NULL)
	{
		char n[128];
		int c;

		if (sscanf(line, "%127s %d", n, &c) == 2 && !strcmp(n, name))
			count = c;
	}
	free(text);
	return count;
}

/* Takes in the feed's index: a package's version, size and address replace the table's, and a
   games package the table has never heard of joins the list as an untested game. Such a game
   stays out of sight until its release folder is known to hold floppy images. */
void catalog_load_feed(const char *text)
{
	const char *block = text;

	catalog_init();
	while (*block)
	{
		const char *end = strstr(block, "\n\n");
		char name[128], v[512], section[64];
		entry_t *e = NULL;
		size_t i;
		int hidden = 0;

		if (!end)
			end = block + strlen(block);
		field(block, end, "Package", name, sizeof name);
		for (i = 0; i < (size_t)n_entries; i++)
			if (!strcmp(entries[i].id, name))
				e = &entries[i];
		for (i = 0; i < sizeof catalog_hidden / sizeof *catalog_hidden; i++)
			hidden |= !strcmp(catalog_hidden[i], name);
		field(block, end, "Section", section, sizeof section);
		if (!e && !hidden && name[0] && strlen(name) < sizeof e->id && !strcmp(section, "Games") &&
		    !strpbrk(name, "/\\\"'$`") && n_entries < (int)(sizeof catalog / sizeof *catalog) + 64)
		{
			char *year, *pub, *f;

			e = &entries[n_entries++];
			memset(e, 0, sizeof *e);
			snprintf(e->id, sizeof e->id, "%s", name);
			field(block, end, "Description", v, sizeof v);  /* "Title (Year) (Publisher) Fnnnnnnn" */
			f = strrchr(v, 'F');
			if (f && strlen(f) >= 8 && strspn(f + 1, "0123456789") >= 7)
				e->game = (unsigned)(atol(f + 1) / 100);
			year = strstr(v, " (");
			pub = year ? strstr(year + 2, " (") : NULL;
			if (year)
				*year = 0;
			snprintf(e->title, sizeof e->title, "%s", v[0] ? v : name);
			if (year)
			{
				year += 2;
				year[strcspn(year, ")")] = 0;
				if (pub)
				{
					pub += 2;
					pub[strcspn(pub, ")")] = 0;
				}
				snprintf(e->info, sizeof e->info, "%s%s%s", year, pub ? ", " : "", pub ? pub : "");
			}
			snprintf(e->boot, sizeof e->boot, "?");         /* worked out from the disc when it arrives */
			e->release = e->floppies = noted_floppies(name) > 0 ? noted_floppies(name) : 0;
		}
		if (e)
		{
			field(block, end, "Version", v, sizeof v);
			if (v[0] && !strpbrk(v, "/\\\"'$` "))
				snprintf(e->version, sizeof e->version, "%s", v);
			field(block, end, "URL", v, sizeof v);
			if (v[0] == '/' && !strpbrk(v, "\"'$`\\ "))
				snprintf(e->url, sizeof e->url, "%s", v);
			if (atol(field(block, end, "Size", v, sizeof v)) > 0)
				e->size = atol(v);
		}
		block = *end ? end + 2 : end;
		while (*block == '\n' || *block == '\r')
			block++;
	}
}

/* Fetches a file with curl. Returns 0 if it arrived. */
int store_fetch(const char *url, const char *dest)
{
	return run_command("curl -fsL --connect-timeout 20 --max-time 900 -o \"%s\" \"%s\"", dest, url);
}

/* Fetches the feed's index into the data folder as feed.txt. Returns 0 if a new copy arrived. */
int feed_refresh(void)
{
	char tmp[1024], dest[1024];
	uint8_t *text;
	int ok;

	snprintf(tmp, sizeof tmp, "%s/feed.new", store_dir());
	snprintf(dest, sizeof dest, "%s/feed.txt", store_dir());
	if (store_fetch(FEED_URL, tmp))
	{
		unlink(tmp);
		return -1;
	}
	text = file_read(tmp, NULL);
	ok = text && strstr((char *)text, "Package:");
	free(text);
	if (ok)
	{
		unlink(dest);
		ok = !rename(tmp, dest);
	}
	unlink(tmp);
	return ok ? 0 : -1;
}

static int release_images(unsigned game, char names[][300], int max);

/* Looks in the release folder of each game the feed has and the table doesn't, and notes in
   floppies.txt how many floppy images are there. A game with none is never listed. This runs
   on the feed's thread: it reads the feed's file for itself and touches nothing the window
   uses. Returns 1 if it noted anything new. */
int feed_check_new(void)
{
	static char names[40][300];
	char path[1024], *text;
	const char *block;
	int noted = 0;

	snprintf(path, sizeof path, "%s/feed.txt", store_dir());
	text = (char *)file_read(path, NULL);
	for (block = text; block && *block; )
	{
		const char *end = strstr(block, "\n\n");
		char name[128], v[512], section[64];
		unsigned game = 0;
		int known = 0, count;
		size_t i;
		char *f;

		if (!end)
			end = block + strlen(block);
		field(block, end, "Package", name, sizeof name);
		field(block, end, "Section", section, sizeof section);
		for (i = 0; i < sizeof catalog / sizeof *catalog; i++)
			known |= !strcmp(catalog[i].id, name);
		for (i = 0; i < sizeof catalog_hidden / sizeof *catalog_hidden; i++)
			known |= !strcmp(catalog_hidden[i], name);
		if (!known && name[0] && !strcmp(section, "Games") && !strpbrk(name, " /") && noted_floppies(name) < 0)
		{
			field(block, end, "Description", v, sizeof v);
			f = strrchr(v, 'F');
			if (f && strlen(f) >= 8 && strspn(f + 1, "0123456789") >= 7)
				game = (unsigned)(atol(f + 1) / 100);
			count = game ? release_images(game, names, 40) : 0;
			if (count >= 0)         /* a listing that didn't arrive is asked for again next time */
			{
				FILE *out;

				snprintf(path, sizeof path, "%s/floppies.txt", store_dir());
				out = fopen(path, "a");
				if (out)
				{
					fprintf(out, "%s %d%c", name, count, 10);
					fclose(out);
					noted = 1;
				}
			}
		}
		block = *end ? end + 2 : end;
		while (*block == 10 || *block == 13)
			block++;
	}
	free(text);
	return noted;
}

static int fail(progress_t *p, const char *why)
{
	snprintf(p->error, sizeof p->error, "%s", why);
	p->done = -1;
	return -1;
}

/* A floppy image's number from its name, which ends F, the game's 5 digits and the disc's 2 */
static int disc_number(const char *name)
{
	size_t n = strlen(name);

	if (n > 4 && (!strcmp(name + n - 4, ".jfd") || !strcmp(name + n - 4, ".JFD")))
		n -= 4;
	if (n >= 8 && name[n - 8] == 'F' && strspn(name + n - 7, "0123456789") >= 7)
		return (name[n - 2] - '0') * 10 + name[n - 1] - '0';
	return 0;
}

/* Works a new game's start and floppy controller out from its first disc, and makes its icon
   from the sprites on its discs */
static void detect(game_t *g)
{
	char path[1024], choices[40][64];

	if (g->ndiscs)
	{
		store_game_path(g->id, g->disc[0], path, sizeof path);
		g->fdc = image_is_hd(path);
	}
	if (store_boot_choices(g, choices, 40))
		snprintf(g->boot, sizeof g->boot, "%s", choices[0]);
	store_pick_icon(g);
}

/* A game's icon: the sprite named after the application it starts from, or failing that the one
   on its discs (or in its HostFS folder) that looks most like an icon */
void store_pick_icon(game_t *g)
{
	sprite_t *sprites;
	int n, i, pick;

	n = store_sprites(g, &sprites);
	pick = n ? sprites_main(sprites, n) : -1;
	for (i = 0; i < n && g->boot[0]; i++)   /* the sprite named after the application, if there is one */
	{
		const char *a = sprites[i].name, *b = g->boot;

		for (; *a && tolower((unsigned char)*a) == tolower((unsigned char)*b); a++, b++);
		if (!*a && !*b)
			pick = i;
	}
	store_set_icon(g, pick >= 0 ? &sprites[pick] : NULL);
	sprites_free(sprites, n);
}

/* Fetches a game's package from the feed and takes the floppy images out of it into the folder
   part. have[n] is set for each disc number that arrives. Returns how many, or -1 if the package
   didn't download. */
static int from_package(const entry_t *e, const char *part, int *have, progress_t *p)
{
	char path[1100], url[600];
	zipent_t *list;
	uint8_t *zip;
	size_t len;
	int i, n, got = 0;

	snprintf(p->step, sizeof p->step, "Downloading %s from JASPP...", e->title);
	p->percent = -1;
	if (e->url[0])
		snprintf(url, sizeof url, PACKAGE_HOST "%s", e->url);
	else
		snprintf(url, sizeof url, PACKAGE_HOST "/packages/release-packages/%s_%s", e->id, e->version);
	snprintf(path, sizeof path, "%s/package.zip", part);
	if (store_fetch(url, path) || !(zip = file_read(path, &len)))
	{
		unlink(path);
		return -1;
	}
	n = zip_list(zip, len, &list);
	for (i = 0; i < n; i++)
	{
		int number = disc_number(base_name(list[i].name));
		uint8_t *data;
		size_t size;

		if (number < 1 || number > 99 || have[number] || !(data = zip_get(zip, len, &list[i], &size)))
			continue;
		snprintf(path, sizeof path, "%s/disc%d.jfd", part, number);
		if (!file_write(path, data, size))
		{
			have[number] = 1;
			got++;
		}
		free(data);
	}
	free(list);
	free(zip);
	snprintf(path, sizeof path, "%s/package.zip", part);
	unlink(path);
	return got;
}

/* The names of the floppy images in a game's release folder on JASPP's server, from the folder's
   listing. Returns how many, or -1 if the listing didn't arrive. Only a plain file name is
   taken, since the names go on a command line. */
static int release_images(unsigned game, char names[][300], int max)
{
	char url[200], *listing, *q;
	int count = 0;

	snprintf(url, sizeof url, RELEASE_URL, game);
	listing = run_output("curl -fsL --connect-timeout 20 --max-time 120 \"%s\"", url);
	if (!listing)
		return -1;
	for (q = listing; (q = strstr(q, "href=\"")) && count < max; )
	{
		size_t len;

		q += 6;
		len = strcspn(q, "\"");
		if (len > 4 && len < 300 && !strncmp(q + len - 4, ".jfd", 4) &&
		    strspn(q, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789%._()!,+~'-") == len)
			snprintf(names[count++], 300, "%.*s", (int)len, q);
	}
	free(listing);
	return count;
}

/* Fetches a game's floppy images from its release folder into the folder part. Returns how many,
   or -1 if one of them didn't arrive. */
static int from_release(const entry_t *e, const char *part, int *have, progress_t *p)
{
	static char names[40][300];
	char path[1100], url[600];
	int count, i, got = 0;

	snprintf(p->step, sizeof p->step, "Looking for %s's floppy images...", e->title);
	p->percent = -1;
	count = release_images(e->game, names, 40);
	for (i = 0; i < count; i++)
	{
		int number = disc_number(names[i]);

		if (number < 1 || number > 99 || have[number])
			continue;
		snprintf(p->step, sizeof p->step, "Downloading %s, disk %d of %d...", e->title, i + 1, count);
		p->percent = i * 100 / count;
		snprintf(url, sizeof url, RELEASE_URL "%s", e->game, names[i]);
		snprintf(path, sizeof path, "%s/disc%d.jfd", part, number);
		if (store_fetch(url, path))
			return -1;
		have[number] = 1;
		got++;
	}
	return count < 0 ? 0 : got;
}

/* Installs a game from the JASPP list. Runs on its own thread, reporting through p. The floppy
   images come from the game's package where that holds the whole set, and otherwise from its
   release folder, which always holds the whole set. A game whose release folder is empty gets
   what its package has. */
int install_jaspp(const entry_t *e, int desktop, int menu, progress_t *p)
{
	char part[1024], final[1024], path[1024], asset[1024];
	int have[100] = { 0 }, i, got = 0, whole = e->discs > 0 && e->discs >= e->set;
	game_t g;

	memset(&g, 0, sizeof g);
	snprintf(g.id, sizeof g.id, "%s.part", e->id);
	store_game_path(g.id, "", part, sizeof part);
	store_game_path(e->id, "", final, sizeof final);
	remove_tree(part);
	if (make_dirs(part))
		return fail(p, "The game's folder can't be made.");

	if (!whole && e->game)
		got = from_release(e, part, have, p);
	if (got == 0 && (whole || e->discs > 0 || !e->tested))
		got = from_package(e, part, have, p);
	if (got < 0)
	{
		remove_tree(part);
		return fail(p, "The download from JASPP didn't arrive. Check the network connection and try again.");
	}
	for (i = 1; i < 100 && g.ndiscs < MAX_DISCS; i++)
		if (have[i])
			snprintf(g.disc[g.ndiscs++], sizeof g.disc[0], "disc%d.jfd", i);
	if (!g.ndiscs)
	{
		remove_tree(part);
		return fail(p, "JASPP has no floppy image for this game.");
	}

	snprintf(g.title, sizeof g.title, "%s", e->title);
	snprintf(g.info, sizeof g.info, "%s", e->info);
	snprintf(g.version, sizeof g.version, "%s", e->version);
	g.jaspp = 1;
	g.scale = 2;
	g.autosave = 1;
	g.desktop = desktop;
	g.menu = menu;
	if (e->tested)
	{
		uint8_t *icon;
		size_t len;

		snprintf(g.boot, sizeof g.boot, "%s", e->boot);
		g.fdc = e->fdc;
		memset(g.ro, e->wp, sizeof g.ro);       /* the games that check for a write protected disc */
		snprintf(path, sizeof path, "icons/%s.png", e->id);
		icon = store_asset(path, asset, sizeof asset) ? file_read(asset, &len) : NULL;
		snprintf(path, sizeof path, "%s/icon.png", part);
		if (icon)
			file_write(path, icon, len);
		free(icon);
		store_save(&g);
	}
	else
	{
		store_save(&g);
		detect(&g);
		store_save(&g);
	}
	/* a game that came with a hard disc installer has the files it copies put on HostFS, and if
	   that fails it plays from its floppies */
	if (run_installer(&g, e->id, p) < 0)
		fprintf(stderr, "piarcboot: %s\n", p->error);
	p->error[0] = 0;
	store_save(&g);
#ifdef _WIN32
	{
		uint8_t rgba[128 * 128 * 4];

		store_icon(&g, rgba);           /* Windows shortcuts take the icon as an .ico file */
		write_icon(g.id, rgba);
	}
#endif

	p->percent = 100;
	snprintf(g.id, sizeof g.id, "%s", e->id);
	if (exists(final))                              /* installing over an older copy keeps its saved discs */
	{
		struct dirent *d;
		DIR *dir = opendir(final);

		while (dir && (d = readdir(dir)))
			if (strlen(d->d_name) > 4 && !strcmp(d->d_name + strlen(d->d_name) - 4, ".sav"))
			{
				char from[1300], to[1300];

				snprintf(from, sizeof from, "%s/%s", final, d->d_name);
				snprintf(to, sizeof to, "%s/%s", part, d->d_name);
				rename(from, to);
			}
		if (dir)
			closedir(dir);
		remove_tree(final);
	}
	if (rename(part, final))
	{
		remove_tree(part);
		return fail(p, "The game's folder can't be put in place.");
	}
	store_shortcuts(&g);
	p->done = 1;
	return 0;
}

static int is_image_name(const char *name)
{
	static const char *const ext[] = { ".jfd", ".adf", ".hfe", ".adl" };
	const char *dot = strrchr(name, '.');
	size_t i, j;

	if (disc_number(name) && !dot)
		return 1;               /* a JASPP image as it is in a package, with no extension */
	for (i = 0; dot && i < sizeof ext / sizeof *ext; i++)
	{
		for (j = 0; dot[j] && tolower((unsigned char)dot[j]) == ext[i][j]; j++);
		if (!dot[j] && !ext[i][j])
			return 1;
	}
	return 0;
}

/* The number of the disc a file's name says it is: JASPP's F<game><disc>, "1 of 3", or a number or
   a letter after Disk, Disc, Side or Part, and failing those a number of 1 or 2 digits at the end
   of the name. 0 if it says none. *at is where the clue starts, so that what comes before it can
   be the game's title, or -1 when it isn't to be cut there (a number at the end may be the
   title's own, as in Lemmings 2). */
int disc_clue(const char *name, int *at)
{
	static const char *const words[] = { "disk", "disc", "side", "part" };
	char low[256];
	int n, i, w, k;

	*at = -1;
	for (n = 0; name[n] && n < 255; n++)
		low[n] = (char)tolower((unsigned char)name[n]);
	low[n] = 0;
	if ((k = disc_number(name)))
	{
		*at = n - (n > 4 && !strcmp(low + n - 4, ".jfd") ? 12 : 8);
		return k;
	}
	if (strrchr(low, '.') && strlen(strrchr(low, '.')) <= 4)
		n = (int)(strrchr(low, '.') - low);
	low[n] = 0;
	for (i = 0; i < n; i++)
	{
		int start = i, num = 0, j;

		for (w = 0; w < 4; w++)         /* Disk 2, disc_2, Side B */
			if (!strncmp(low + i, words[w], 4) && (!i || !isalpha((unsigned char)low[i - 1])))
			{
				for (j = i + 4; strchr(" _-#.", low[j]) && low[j]; j++);
				if (isdigit((unsigned char)low[j]))
					num = atoi(low + j);
				else if (low[j] >= 'a' && low[j] <= 'h' && !isalnum((unsigned char)low[j + 1]))
					num = low[j] - 'a' + 1;
				break;
			}
		if (!num && isdigit((unsigned char)low[i]) && (!i || !isdigit((unsigned char)low[i - 1])))
		{
			for (j = i; isdigit((unsigned char)low[j]); j++);
			for (k = j; low[k] == ' ' || low[k] == '_'; k++);
			if (!strncmp(low + k, "of", 2))         /* 1 of 3, 1_of_3 */
			{
				for (k += 2; low[k] == ' ' || low[k] == '_'; k++);
				if (isdigit((unsigned char)low[k]))
				{
					num = atoi(low + i);
					for (w = 0; w < 4; w++)         /* "Disk 1 of 3" starts at Disk */
						if (start >= 5 && !strncmp(low + start - 5, words[w], 4))
							start -= 5;
				}
			}
		}
		if (num > 0 && num < 100)
		{
			while (start > 0 && strchr(" ([-_", low[start - 1]))
				start--;
			*at = start;
			return num;
		}
	}
	for (i = n; i > 0 && isdigit((unsigned char)low[i - 1]); i--);
	return n - i >= 1 && n - i <= 2 && i > 0 ? atoi(low + i) : 0;
}

/* Disc images in the order their names give, as far as they give one */
static int by_name(const void *a, const void *b)
{
	const char *s = base_name(*(char *const *)a), *t = base_name(*(char *const *)b);
	int at, ds = disc_clue(s, &at), dt = disc_clue(t, &at);

	return ds && dt && ds != dt ? ds - dt : strcmp(s, t);
}

/* A new game named after a file of it: "Name (1995) (Publisher) F1001501.jfd" gives the title and
   the line under it, and the title a folder name no other game has */
static void new_game(game_t *g, const char *file)
{
	char title[128], path[1300], *p;
	int i, k;

	memset(g, 0, sizeof *g);
	snprintf(title, sizeof title, "%s", base_name(file));
	k = (int)strlen(title);
	if (k > 4 && title[k - 4] == ',' && strspn(title + k - 3, "0123456789abcdefABCDEF") == 3)
		title[k - 4] = 0;               /* a RISC OS type after a comma, as in Game,ddc */
	if (disc_clue(title, &k) && k >= 0)     /* "Name (Disk 1 of 3).adf" is called Name */
		title[k] = 0;
	else if ((p = strrchr(title, '.')))
		*p = 0;
	if ((p = strstr(title, " (")))
	{
		char *year = p + 2, *pub = strstr(year, " (");

		*p = 0;
		year[strcspn(year, ")")] = 0;
		if (pub)
			pub[2 + strcspn(pub + 2, ")")] = 0;
		if (strlen(year) == 4 && strspn(year, "0123456789") == 4)       /* not "(Disc 1)" */
			snprintf(g->info, sizeof g->info, "%s%s%s", year, pub ? ", " : "", pub ? pub + 2 : "");
	}
	for (p = title + strlen(title); p > title && (p[-1] == ' ' || p[-1] == '_' || p[-1] == '-'); )
		*--p = 0;
	snprintf(g->title, sizeof g->title, "%s", title[0] ? title : "Game");
	for (i = 0, k = 0; g->title[i] && k < 40; i++)
		if (isalnum((unsigned char)g->title[i]))
			g->id[k++] = g->title[i];
		else if (k && g->id[k - 1] != '-')
			g->id[k++] = '-';
	if (!k)
		snprintf(g->id, sizeof g->id, "Game");
	for (i = 2; ; i++)              /* a folder name no other game has */
	{
		store_game_path(g->id, "", path, sizeof path);
		if (!exists(path))
			break;
		snprintf(g->id + k, sizeof g->id - k, "-%d", i);
	}
	g->scale = 2;
	g->autosave = 1;
}

/* ---- Archives of applications ----

   A zip of RISC OS files rather than disc images, or a Spark or ArcFS archive, is laid out as a
   Classics game is: the folder its applications are in made the top, on floppy images, or in a
   HostFS folder when it is too big for floppies or its applications' !Run files ask for what the
   support pack gives. It starts from the application named after the archive, or failing that its
   biggest, and as a Classics game does, on the desktop at once. */

static int is_zip(const uint8_t *d, size_t len)
{
	return len >= 4 && !memcmp(d, "PK\3\4", 4);
}

/* 1 for a zip with no disc images in it, or a Spark or ArcFS archive */
static int is_app_archive(const char *file)
{
	size_t len;
	uint8_t *d = file_read(file, &len);
	int is = 0;

	if (d && is_zip(d, len))
	{
		zipent_t *ents;
		int m = zip_list(d, len, &ents), k;

		is = m > 0;
		for (k = 0; k < m; k++)
			is &= !is_image_name(base_name(ents[k].name));
		if (m > 0)
			free(ents);
	}
	else if (d)
		is = arc_is_archive(d, len);
	free(d);
	return is;
}

/* The application the game starts from: the one named after the game, or the biggest */
static void archive_boot(const adfs_file_t *f, int n, const char *title, char *boot, size_t size)
{
	char names[64][64];
	uint32_t sizes[64];
	int count = 0, i, k, best = -1;

	for (i = 0; i < n; i++)
	{
		size_t len = strcspn(f[i].path, ".");

		if (f[i].path[0] != '!' || len >= 64)
			continue;
		for (k = 0; k < count && (strlen(names[k]) != len || strncmp(names[k], f[i].path, len)); k++);
		if (k == count)
		{
			if (count == 64)
				continue;
			snprintf(names[count], 64, "%.*s", (int)len, f[i].path);
			sizes[count++] = 0;
		}
		sizes[k] += f[i].len;
	}
	for (k = 0; k < count; k++)
	{
		if (is_system(names[k]))
			continue;
		if (!by_choice(names[k] + 1, title))
		{
			best = k;
			break;
		}
		if (best < 0 || sizes[k] > sizes[best])
			best = k;
	}
	snprintf(boot, size, "%s", best >= 0 ? names[best] : "");
}

static int import_apps(char **files, int n, char *id_out, char *error, size_t error_size)
{
	char dir[1300];
	adfs_file_t *all = NULL;
	int total = 0, i;
	progress_t p;
	game_t g;

	for (i = 0; i < n; i++)
	{
		adfs_file_t *f = NULL;
		size_t len;
		uint8_t *d = file_read(files[i], &len);
		int m = !d ? -1 : is_zip(d, len) ? adfs_from_zip(d, len, "", &f) : arc_files(d, len, &f, error, error_size);

		if (m < 0)
		{
			if (!d)
				snprintf(error, error_size, "%s can't be read.", base_name(files[i]));
			free(d);
			adfs_free(all, total);
			return -1;
		}
		free(d);
		all = realloc(all, (size_t)(total + m + 1) * sizeof *all);
		if (m)
			memcpy(all + total, f, (size_t)m * sizeof *f);
		free(f);
		total += m;
	}
	total = adfs_app_root(all, total);
	new_game(&g, files[0]);
	archive_boot(all, total, g.title, g.boot, sizeof g.boot);
	if (!g.boot[0])
	{
		snprintf(error, error_size, "There's no application in that archive to start a game from.");
		adfs_free(all, total);
		return -1;
	}
	g.apps = 1;
	store_game_path(g.id, "", dir, sizeof dir);
	make_dirs(dir);
	memset(&p, 0, sizeof p);
	if (layout_files(&g, dir, all, total, g.boot, needs_support(all, total), &p))
	{
		snprintf(error, error_size, "%s", p.error);
		adfs_free(all, total);
		remove_tree(dir);
		return -1;
	}
	adfs_free(all, total);
	store_save(&g);
	store_pick_icon(&g);
	store_shortcuts(&g);
	if (id_out)
		snprintf(id_out, sizeof g.id, "%s", g.id);
	return 0;
}

/* Makes a game from disc images of the user's own, or with add_to adds the images to the game of
   that name. A zip file among them stands for the disc images inside it, and archives of
   applications make a game of their own (above). The new game's name is put in id_out. Returns
   0, or -1 with the reason in error. */
int import_game(char **files, int n, const char *add_to, char *id_out, char *error, size_t error_size)
{
	char *list[64], tmp[1024], path[1300];
	int count = 0, made = 0, i, k, order[MAX_DISCS], sorted = 1, apps = 0;
	game_t g;

	for (i = 0; i < n; i++)
		apps += is_app_archive(files[i]);
	if (apps)               /* archives of applications make a game of their own */
	{
		if (apps < n)
		{
			snprintf(error, error_size, "Add disc images and archives of applications separately.");
			return -1;
		}
		return import_apps(files, n, id_out, error, error_size);
	}
	snprintf(tmp, sizeof tmp, "%s/import.tmp", store_dir());
	remove_tree(tmp);
	for (i = 0; i < n && count < 64; i++)
	{
		const char *dot = strrchr(files[i], '.');

		if (dot && (!strcmp(dot, ".zip") || !strcmp(dot, ".ZIP")))
		{
			zipent_t *ents;
			size_t len, size;
			uint8_t *zip = file_read(files[i], &len), *data;
			int m = zip ? zip_list(zip, len, &ents) : 0;

			make_dirs(tmp);
			for (k = 0; k < m && count < 64; k++)
				if (is_image_name(base_name(ents[k].name)) && (data = zip_get(zip, len, &ents[k], &size)))
				{
					snprintf(path, sizeof path, "%s/%s", tmp, base_name(ents[k].name));
					if (!file_write(path, data, size))
						list[count++] = strdup(path);
					free(data);
					made = 1;
				}
			if (m)
				free(ents);
			free(zip);
		}
		else if (is_image_name(base_name(files[i])))
			list[count++] = strdup(files[i]);
	}
	if (!count)
	{
		snprintf(error, error_size, "None of those files is a disc image or an archive of applications. PiArcBoot reads JFD, ADF, ADL and HFE images, and zip, Spark and ArcFS archives.");
		remove_tree(tmp);
		return -1;
	}
	qsort(list, count, sizeof *list, by_name);

	if (!add_to || store_load(add_to, &g))
		new_game(&g, list[0]);

	store_game_path(g.id, "", path, sizeof path);
	make_dirs(path);
	for (i = 0; i < count && g.ndiscs < MAX_DISCS; i++)
	{
		const char *dot = strrchr(base_name(list[i]), '.');
		char ext[8] = ".jfd", name[40];
		uint8_t *data;
		size_t len;

		if (dot && strlen(dot) < sizeof ext)
			for (k = 0; (ext[k] = tolower((unsigned char)dot[k])); k++);
		for (k = g.ndiscs + 1; ; k++)
		{
			snprintf(name, sizeof name, "disc%d%s", k, ext);
			store_game_path(g.id, name, path, sizeof path);
			if (!exists(path))
				break;
		}
		data = file_read(list[i], &len);
		if (data && !file_write(path, data, len))
			snprintf(g.disc[g.ndiscs++], sizeof g.disc[0], "%s", name);
		free(data);
	}
	for (i = 0; i < count; i++)
		free(list[i]);
	if (made)
		remove_tree(tmp);
	if (!g.ndiscs)
	{
		snprintf(error, error_size, "The disc images couldn't be copied.");
		return -1;
	}
	/* JASPP images say which disc of the set they are, and go in that order however they arrived */
	for (i = 0; i < g.ndiscs; i++)
	{
		size_t len;
		uint8_t *d;

		store_game_path(g.id, g.disc[i], path, sizeof path);
		d = jfd_read(path, &len);
		order[i] = d && len > 16 && !memcmp(d, "JFDI", 4) ? d[12] | d[13] << 8 : 0;
		free(d);
		sorted &= order[i] > 0;
		for (k = 0; k < i; k++)
			sorted &= order[k] != order[i];
	}
	for (i = 1; sorted && i < g.ndiscs; i++)
		for (k = i; k > 0 && order[k - 1] > order[k]; k--)
		{
			char name[40];
			int t = order[k];

			order[k] = order[k - 1];
			order[k - 1] = t;
			memcpy(name, g.disc[k], sizeof name);
			memcpy(g.disc[k], g.disc[k - 1], sizeof name);
			memcpy(g.disc[k - 1], name, sizeof name);
		}

	store_save(&g);
	if (!add_to || (!g.sprite[0] && !g.jaspp))      /* a game whose icon nobody picked gets the best one going */
	{
		char boot[64];

		memcpy(boot, g.boot, sizeof boot);
		detect(&g);
		if (add_to && boot[0])
			memcpy(g.boot, boot, sizeof boot);      /* a start that was chosen by hand stays */
		store_save(&g);
	}
	store_shortcuts(&g);
	if (id_out)
		snprintf(id_out, sizeof g.id, "%s", g.id);
	return 0;
}

/* Starts a game in a window of its own, as "piarcboot play <game>" */
int spawn_game(const char *id)
{
#ifdef _WIN32
	STARTUPINFOA si = { sizeof si };
	PROCESS_INFORMATION pi;
	char cmd[1200];

	snprintf(cmd, sizeof cmd, "\"%s\" play \"%s\"", store_exe(), id);
	if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, DETACHED_PROCESS, NULL, NULL, &si, &pi))
		return -1;
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);
	return 0;
#else
	/* The game runs as a grandchild, which nobody waits for and which leaves nothing behind, so
	   that the commands the launcher waits for are its only children */
	pid_t pid = fork();

	if (pid == 0)
	{
		setsid();
		if ((pid = fork()) == 0)
		{
			execl(store_exe(), store_exe(), "play", id, (char *)NULL);
			_exit(127);
		}
		_exit(pid < 0 ? 127 : 0);
	}
	if (pid > 0)
		waitpid(pid, NULL, 0);
	return pid < 0 ? -1 : 0;
#endif
}

/* Asks for disc images with the system's own file dialog. Returns the names chosen, to be
   freed, with *n how many: 0 if the dialog was cancelled, -1 if there is no dialog to ask with. */
char **pick_files(int *n)
{
	char **list = NULL;

	*n = 0;
#ifdef _WIN32
	{
		static char buf[32768];
		OPENFILENAMEA ofn = { sizeof ofn };
		char *p;

		buf[0] = 0;
		ofn.lpstrFilter = "Disc images and archives\0*.jfd;*.adf;*.hfe;*.adl;*.zip;*.arc;*.spk\0All files\0*.*\0";
		ofn.lpstrFile = buf;
		ofn.nMaxFile = sizeof buf;
		ofn.lpstrTitle = "Add Game: choose its disc images, or an archive of its applications";
		ofn.Flags = OFN_ALLOWMULTISELECT | OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
		if (!GetOpenFileNameA(&ofn))
			return NULL;
		p = buf + strlen(buf) + 1;
		if (!*p)                /* one file: the whole path */
		{
			list = malloc(sizeof *list);
			list[(*n)++] = strdup(buf);
		}
		for (; *p; p += strlen(p) + 1)  /* several: the folder, then each name */
		{
			char path[1024];

			snprintf(path, sizeof path, "%s\\%s", buf, p);
			list = realloc(list, (*n + 1) * sizeof *list);
			list[(*n)++] = strdup(path);
		}
	}
#else
	{
		char *out, *p;

		out = run_output("zenity --file-selection --multiple --separator='\n' --title='Add Game: choose its disc images, or an archive of its applications' "
				 "--file-filter='Disc images and archives | *.jfd *.JFD *.adf *.ADF *.hfe *.HFE *.adl *.ADL *.zip *.ZIP *.arc *.ARC *.spk *.SPK *,ddc *,3fb *,a91' "
				 "--file-filter='All files | *' 2>/dev/null");
		if (!out && run_command("command -v zenity >/dev/null 2>&1"))
		{
			out = run_output("kdialog --getopenfilename --multiple --separate-output . 2>/dev/null");
			if (!out && run_command("command -v kdialog >/dev/null 2>&1"))
			{
				*n = -1;
				return NULL;
			}
		}
		for (p = out; p && *p; )
		{
			size_t len = strcspn(p, "\n");

			if (len)
			{
				list = realloc(list, (*n + 1) * sizeof *list);
				list[*n] = malloc(len + 1);
				memcpy(list[*n], p, len);
				list[(*n)++][len] = 0;
			}
			p += len + (p[len] == '\n');
		}
		free(out);
	}
#endif
	return list;
}

void open_url(const char *url)
{
#ifdef _WIN32
	ShellExecuteA(NULL, "open", url, NULL, NULL, SW_SHOWNORMAL);
#else
	run_command("xdg-open \"%s\" >/dev/null 2>&1 &", url);
#endif
}
