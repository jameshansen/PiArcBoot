/* The installer, "piarcboot setup": pages drawn with ROOGUI that copy the PiArcBoot folder this
   program is in to where PiArcBoot is to live, make its shortcuts, and update or remove a copy
   installed before. The release's installer unpacks the folder to a temporary one and runs this
   from there. With --quiet, or no desktop to show the pages on, it installs or updates with the
   usual settings and says what it did on the terminal. Games and saves are in the data folder
   (store_dir), which it never touches. */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <SDL.h>
#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#else
#include <sys/statvfs.h>
#include <sys/wait.h>
#endif
#include "roogui.h"
#include "piarcboot.h"

#define W 620
#define H 420
#define PANEL_W 180
#define BAR_H 50
#define BTN_W 90
#define BTN_H 26

#ifdef _WIN32
#define SEP "\\"
#define EXE "piarcboot.exe"
#define MENU_NAME "Start menu"
#else
#define SEP "/"
#define EXE "piarcboot"
#define MENU_NAME "Games menu"
#endif
#if defined(__linux__) && defined(__aarch64__)
#define MACHINE "Raspberry Pi"
#else
#define MACHINE "computer"
#endif

enum { P_WELCOME, P_FOUND, P_FOLDER, P_READY, P_WORKING, P_DONE };
enum { A_INSTALL, A_UPDATE, A_REMOVE };

static char src[700];           /* the folder this program is in: what is installed */
static char dest[700];          /* where it goes, or the copy being updated or removed */
static char found[700], found_version[32];     /* a copy installed before, and its version if known */
static int desktop = 1, menu = 1, action = A_INSTALL;
static volatile int cancel;
static volatile long long total, copied;
static progress_t prog;

static int exists(const char *path)
{
	struct stat st;

	return !stat(path, &st);
}

/* Where PiArcBoot goes unless the user says otherwise. With PIARCBOOT_HOME set, which the tests
   do, inside that folder. */
static void default_dir(char *buf, size_t size)
{
	const char *test = getenv("PIARCBOOT_HOME");

	if (test)
		snprintf(buf, size, "%s" SEP "Programs" SEP "PiArcBoot", test);
	else
#ifdef _WIN32
		snprintf(buf, size, "%s\\Programs\\PiArcBoot", getenv("LOCALAPPDATA") ? getenv("LOCALAPPDATA") : ".");
#else
		snprintf(buf, size, "%s/Games/PiArcBoot", getenv("HOME") ? getenv("HOME") : ".");
#endif
}

/* The shortcut on the desktop and the one in the menu */
static void shortcut_paths(char *desk, char *start, size_t size)
{
#ifdef _WIN32
	char dir[MAX_PATH];

	if (getenv("PIARCBOOT_HOME"))
	{
		snprintf(desk, size, "%s\\desktop\\PiArcBoot.lnk", getenv("PIARCBOOT_HOME"));
		snprintf(start, size, "%s\\menu\\PiArcBoot.lnk", getenv("PIARCBOOT_HOME"));
		return;
	}
	if (SHGetFolderPathA(NULL, CSIDL_DESKTOPDIRECTORY, NULL, 0, dir) != S_OK)
		snprintf(dir, sizeof dir, "%s\\Desktop", getenv("USERPROFILE") ? getenv("USERPROFILE") : ".");
	snprintf(desk, size, "%s\\PiArcBoot.lnk", dir);
	if (SHGetFolderPathA(NULL, CSIDL_PROGRAMS, NULL, 0, dir) != S_OK)
		snprintf(dir, sizeof dir, "%s\\Microsoft\\Windows\\Start Menu\\Programs", getenv("APPDATA") ? getenv("APPDATA") : ".");
	snprintf(start, size, "%s\\PiArcBoot.lnk", dir);
#else
	char apps[600], deskdir[600];

	store_shortcut_dirs(apps, sizeof apps, deskdir, sizeof deskdir);
	snprintf(desk, size, "%s/piarcboot.desktop", deskdir);
	snprintf(start, size, "%s/piarcboot.desktop", apps);
#endif
}

/* The shortcuts the copy installed before has, which an update keeps as they are */
static void has_shortcuts(void)
{
	char desk[800], start[800];

	shortcut_paths(desk, start, sizeof desk);
	desktop = exists(desk);
#ifdef _WIN32
	menu = exists(start);
#else
	{
		char *d = (char *)file_read(start, NULL);

		menu = d && !strstr(d, "NoDisplay=true");       /* hidden, it is only there for the taskbar's icon */
		free(d);
	}
#endif
}

#ifdef _WIN32
static void ps_quoted(FILE *f, const char *s)
{
	fputc('\'', f);
	for (; *s; s++)
		fprintf(f, *s == '\'' ? "''" : "%c", *s);
	fputc('\'', f);
}
#endif

#ifndef _WIN32
/* The program in a folder, as the quoted first word of a desktop entry's Exec line. In quotes ",
   ` and $ take a backslash and a backslash takes three more (the value's own escaping comes
   first), and % is doubled. */
static void exec_quoted(FILE *f, const char *dir)
{
	const char *s;

	fputc('"', f);
	for (s = dir; *s; s++)
		if (*s == '\\')
			fputs("\\\\\\\\", f);
		else if (*s == '"' || *s == '`' || *s == '$')
			fprintf(f, "\\%c", *s);
		else if (*s == '%')
			fputs("%%", f);
		else
			fputc(*s, f);
	fputs("/piarcboot\"", f);
}
#endif

/* Makes the shortcuts match what is wanted, pointing at the program in dest */
static void make_shortcuts(int want_desktop, int want_menu)
{
	char desk[800], start[800];

	shortcut_paths(desk, start, sizeof desk);
#ifdef _WIN32
	{
		char ps1[MAX_PATH + 40], exe[800], dir[800];
		FILE *f;
		int i;

		for (i = 0; i < 2; i++)
		{
			snprintf(dir, sizeof dir, "%s", i ? start : desk);
			dir[base_name(dir) - dir] = 0;
			if ((i ? want_menu : want_desktop))
				make_dirs(dir);
			else
				DeleteFileA(i ? start : desk);
		}
		if (!want_desktop && !want_menu)
			return;
		snprintf(exe, sizeof exe, "%s\\%s", dest, EXE);
		GetTempPathA(MAX_PATH, ps1);
		strcat(ps1, "piarcboot-shortcuts.ps1");
		if (!(f = fopen(ps1, "w")))
			return;
		for (i = 0; i < 2; i++)
			if (i ? want_menu : want_desktop)
			{
				fprintf(f, "$s = (New-Object -ComObject WScript.Shell).CreateShortcut(");
				ps_quoted(f, i ? start : desk);
				fprintf(f, ")\n$s.TargetPath = ");
				ps_quoted(f, exe);
				fprintf(f, "\n$s.WorkingDirectory = ");
				ps_quoted(f, dest);
				fprintf(f, "\n$s.Save()\n");
			}
		fclose(f);
		run_command("powershell -NoProfile -ExecutionPolicy Bypass -File \"%s\"", ps1);
		DeleteFileA(ps1);
	}
#else
	{
		char dir[800];
		int i;

		/* The menu entry is there even when it isn't wanted, hidden, since the taskbar looks the
		   launcher's icon up through it; it carries the name the launcher's window goes by */
		for (i = 0; i < 2; i++)
		{
			const char *path = i ? desk : start;
			FILE *f;

			if ((i && !want_desktop) || (!i && action == A_REMOVE))
			{
				unlink(path);
				continue;
			}
			snprintf(dir, sizeof dir, "%s", path);
			dir[base_name(dir) - dir] = 0;
			make_dirs(dir);
			if (!(f = fopen(path, "w")))
				continue;
			fprintf(f, "[Desktop Entry]\nType=Application\nName=PiArcBoot\nComment=Acorn Archimedes games, booted straight from their disc images\nExec=");
			exec_quoted(f, dest);
			fprintf(f, "\nIcon=%s/icon.png\nTerminal=false\nCategories=Game;\nStartupWMClass=piarcboot\n%s",
				dest, !i && !want_menu ? "NoDisplay=true\n" : "");
			fclose(f);
			chmod(path, 0755);
		}
	}
#endif
}

/* The record of where PiArcBoot was installed and which version, kept in the data folder */
static void record_path(char *buf, size_t size)
{
	make_dirs(store_home_dir());            /* the installed copy's data folder, even run from a portable one */
	snprintf(buf, size, "%s/installed", store_home_dir());
}

static void find_installed(void)
{
	char rec[800], exe[800], *d;

	record_path(rec, sizeof rec);
	found[0] = found_version[0] = 0;
	if ((d = (char *)file_read(rec, NULL)))
	{
		char *nl = strchr(d, '\n');

		if (nl)
		{
			snprintf(found, sizeof found, "%.*s", (int)strcspn(d, "\r\n"), d);
			snprintf(found_version, sizeof found_version, "%.*s", (int)strcspn(nl + 1, "\r\n"), nl + 1);
		}
		free(d);
	}
	snprintf(exe, sizeof exe, "%s" SEP EXE, found);
	if (!found[0] || !exists(exe))          /* a copy installed before setup kept a record, or one moved since */
	{
		default_dir(found, sizeof found);
		found_version[0] = 0;
		snprintf(exe, sizeof exe, "%s" SEP EXE, found);
		if (!exists(exe))
			found[0] = 0;
	}
}

/* Two versions by their numbers: -1, 0 or 1 as a is earlier than, the same as or later than b.
   One that isn't three numbers, such as a test build's, counts as the same. */
static int version_cmp(const char *a, const char *b)
{
	int x[3], y[3], i;

	if (sscanf(a, "%d.%d.%d", &x[0], &x[1], &x[2]) != 3 || sscanf(b, "%d.%d.%d", &y[0], &y[1], &y[2]) != 3)
		return 0;
	for (i = 0; i < 3; i++)
		if (x[i] != y[i])
			return x[i] < y[i] ? -1 : 1;
	return 0;
}

static long long tree_size(const char *dir, int top)
{
	DIR *d = opendir(dir);
	struct dirent *e;
	char path[1024];
	long long n = 0;
	struct stat st;

	while (d && (e = readdir(d)))
		if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..") && !(top && !strcmp(e->d_name, "Data")))
		{
			snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
			if (is_dir(path))
				n += tree_size(path, 0);
			else if (!stat(path, &st))
				n += st.st_size;
		}
	if (d)
		closedir(d);
	return n;
}

/* The folder copied, but for the Data folder a portable copy keeps its games in. 0 when it all copied. */
static int copy_dir(const char *from, const char *to, int top)
{
	DIR *d = opendir(from);
	struct dirent *e;
	char a[1024], b[1024];
	int bad = !d || make_dirs(to);

	while (!bad && !cancel && (e = readdir(d)))
		if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..") && !(top && !strcmp(e->d_name, "Data")))
		{
			snprintf(a, sizeof a, "%s/%s", from, e->d_name);
			snprintf(b, sizeof b, "%s/%s", to, e->d_name);
			if (is_dir(a))
				bad = copy_dir(a, b, 0);
			else
			{
				size_t len;
				uint8_t *data;
				struct stat st;

				snprintf(prog.step, sizeof prog.step, "Copying %s", a + strlen(src) + 1);
				data = file_read(a, &len);
				bad = !data || file_write(b, data, len);
				free(data);
#ifndef _WIN32
				if (!stat(a, &st))
					chmod(b, st.st_mode & 0777);
#else
				(void)st;
#endif
				copied += len;
				prog.percent = total ? (int)(copied * 100 / total) : 100;
			}
		}
	if (d)
		closedir(d);
	return bad || cancel;
}

static int failed(const char *fmt, const char *arg)
{
	snprintf(prog.error, sizeof prog.error, fmt, arg);
	prog.done = -1;
	return -1;
}

/* PiArcBoot's own files at the top of its folder: what the installer brings, and what earlier
   releases left there. Anything else in the folder, such as a portable copy's Data folder, is the
   user's, and installing, updating and removing leave it where it is. */
static int own_file(const char *name)
{
	char path[1024];

	if (!strcmp(name, "Data"))
		return 0;
	if (!strcmp(name, "install.sh") || !strcmp(name, "piarcboot.pdb"))
		return 1;
	snprintf(path, sizeof path, "%s" SEP "%s", src, name);
	return exists(path);
}

/* The names at the top of a folder, PiArcBoot's own alone when own is set. Returns how many, up
   to max. */
static int entries(const char *dir, int own, char names[][256], int max)
{
	DIR *d = opendir(dir);
	struct dirent *e;
	int n = 0;

	while (d && n < max && (e = readdir(d)))
		if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..") && (!own || own_file(e->d_name)))
			snprintf(names[n++], 256, "%s", e->d_name);
	if (d)
		closedir(d);
	return n;
}

/* Moves those names from one folder to another. Returns how many moved before one didn't. */
static int move_entries(const char *from, const char *to, char names[][256], int n)
{
	char a[1100], b[1100];
	int i;

	for (i = 0; i < n; i++)
	{
		snprintf(a, sizeof a, "%s" SEP "%s", from, names[i]);
		snprintf(b, sizeof b, "%s" SEP "%s", to, names[i]);
		if (rename(a, b))
			break;
	}
	return i;
}

/* The copy of PiArcBoot in a folder is running. Windows lets a running program be renamed but not
   replaced or deleted, so it is asked first; elsewhere a running program can be replaced. */
static int in_use(const char *dir)
{
#ifdef _WIN32
	char exe[800];
	HANDLE h;

	snprintf(exe, sizeof exe, "%s\\" EXE, dir);
	h = CreateFileA(exe, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
	if (h == INVALID_HANDLE_VALUE)
		return GetLastError() == ERROR_SHARING_VIOLATION;
	CloseHandle(h);
#else
	(void)dir;
#endif
	return 0;
}

#define MAX_ENTRIES 256

/* Installs, updates or removes, as action says. The new copy is put together beside the old one,
   then the old copy's own files go aside and the new ones go in, so a cancelled or failed install
   leaves things as they were. prog.done is 1 when it is finished, -1 failed (why in prog.error),
   -2 cancelled. */
static int work(void *unused)
{
	static char names[MAX_ENTRIES][256], back[MAX_ENTRIES][256];
	char stage[800], old[800], rec[800];
	int n, moved = 0;
	FILE *f;

	(void)unused;
	record_path(rec, sizeof rec);
	if (in_use(dest))
		return failed("PiArcBoot in %s is in use. Close it, then try again.", dest);
	if (action == A_REMOVE)
	{
		snprintf(prog.step, sizeof prog.step, "Removing %s", dest);
		n = entries(dest, 1, names, MAX_ENTRIES);
		while (n-- > 0)
		{
			snprintf(stage, sizeof stage, "%s" SEP "%s", dest, names[n]);
			remove_tree(stage);
		}
		rmdir(dest);            /* only if nothing of the user's is left in it */
		make_shortcuts(0, 0);
		remove(rec);
		prog.percent = 100;
		prog.done = 1;
		return 0;
	}
	snprintf(stage, sizeof stage, "%s.new", dest);
	snprintf(old, sizeof old, "%s.old", dest);
	remove_tree(stage);
	remove_tree(old);
	total = tree_size(src, 1);
	copied = 0;
	if (copy_dir(src, stage, 1))
	{
		remove_tree(stage);
		if (cancel)
		{
			prog.done = -2;
			return 0;
		}
		return failed("PiArcBoot couldn't be copied to %s. Check there is room there and that you can write to it.", dest);
	}
	snprintf(prog.step, sizeof prog.step, "Adding the shortcuts");
	n = entries(dest, 1, names, MAX_ENTRIES);
	if (make_dirs(dest) || make_dirs(old) || (moved = move_entries(dest, old, names, n)) < n)
	{
		if (is_dir(old) && move_entries(old, dest, names, moved) == moved)
			remove_tree(old);
		remove_tree(stage);
		return failed("PiArcBoot couldn't be put in %s. Check that you can write to it.", dest);
	}
	n = entries(stage, 0, back, MAX_ENTRIES);
	if (move_entries(stage, dest, back, n) < n)
		return failed("PiArcBoot couldn't be put in %s. The copy that was there is in the folder beside it with .old on its name.", dest);
	remove_tree(old);
	rmdir(stage);
	make_shortcuts(desktop, menu);
	if ((f = fopen(rec, "wb")))
	{
		fprintf(f, "%s\n%s\n", dest, PIARCBOOT_VERSION);
		fclose(f);
	}
	prog.percent = 100;
	prog.done = 1;
	return 0;
}

/* A folder the user typed or picked that can be installed in: a new one, an empty one, or one
   that holds PiArcBoot. NULL if it will do, otherwise why not. */
static const char *bad_folder(const char *dir)
{
	char exe[800];
	DIR *d;
	struct dirent *e;
	int empty = 1;

#ifdef _WIN32
	if (!((dir[0] && dir[1] == ':' && (dir[2] == '\\' || dir[2] == '/')) || (dir[0] == '\\' && dir[1] == '\\')))
#else
	if (dir[0] != '/')
#endif
		return "Choose a full folder path, starting from the top of the drive.";
	if (!strcmp(dir, src))
		return "That is the folder the installer is running from. Choose another.";
	snprintf(exe, sizeof exe, "%s" SEP EXE, dir);
	if (!is_dir(dir) || exists(exe))
		return NULL;
	if ((d = opendir(dir)))
	{
		while ((e = readdir(d)))
			if (strcmp(e->d_name, ".") && strcmp(e->d_name, ".."))
				empty = 0;
		closedir(d);
	}
	return empty ? NULL : "That folder already has other files in it. Choose a new folder or an empty one.";
}

static long long free_space(const char *dir)
{
	char p[700], *s;

	snprintf(p, sizeof p, "%s", dir);
	while (p[0] && !is_dir(p) && (s = strrchr(p, SEP[0])) && s > p)
		*s = 0;
#ifdef _WIN32
	{
		ULARGE_INTEGER n;

		if (strlen(p) == 2)
			strcat(p, "\\");
		if (GetDiskFreeSpaceExA(p, &n, NULL, NULL))
			return (long long)n.QuadPart;
	}
#else
	{
		struct statvfs v;

		if (!statvfs(p, &v))
			return (long long)v.f_bavail * v.f_frsize;
	}
#endif
	return -1;
}

/* The system's folder picker, for the Change button. 0 if a folder was picked. */
static int pick_folder(char *buf, size_t size)
{
	char dir[1024];

#ifdef _WIN32
	BROWSEINFOA bi = { 0 };
	LPITEMIDLIST id;

	CoInitialize(NULL);
	bi.lpszTitle = "Select Install Location";
	bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
	if (!(id = SHBrowseForFolderA(&bi)) || !SHGetPathFromIDListA(id, dir))
		return -1;
#else
	char *out = run_output("zenity --file-selection --directory --title='Select Install Location' 2>/dev/null");

	if (!out)
		return -1;
	out[strcspn(out, "\r\n")] = 0;
	snprintf(dir, sizeof dir, "%s", out);
	free(out);
	if (dir[0] != '/')
		return -1;
#endif
	if (dir[strlen(dir) - 1] == SEP[0])
		dir[strlen(dir) - 1] = 0;
	if (strcmp(base_name(dir), "PiArcBoot"))
		snprintf(buf, size, "%s" SEP "PiArcBoot", dir);
	else
		snprintf(buf, size, "%s", dir);
	return 0;
}

/* The copy just installed started, as a program of its own that outlives this one */
static void start_installed(void)
{
	char exe[800];

	snprintf(exe, sizeof exe, "%s" SEP EXE, dest);
#ifdef _WIN32
	{
		STARTUPINFOA si = { sizeof si };
		PROCESS_INFORMATION pi;
		char cmd[820];

		snprintf(cmd, sizeof cmd, "\"%s\"", exe);
		if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, DETACHED_PROCESS, NULL, dest, &si, &pi))
		{
			CloseHandle(pi.hProcess);
			CloseHandle(pi.hThread);
		}
	}
#else
	{
		pid_t pid = fork();

		if (pid == 0)
		{
			setsid();
			if (fork() == 0)
			{
				execl(exe, exe, (char *)NULL);
				_exit(127);
			}
			_exit(0);
		}
		if (pid > 0)
			waitpid(pid, NULL, 0);
	}
#endif
}

/* The pages */

static SDL_Texture *logo;

static int wrap(int font, int x, int y, int w, uint32_t rgb, const char *s)
{
	char line[400], next[400];
	int lh = rg_font_height(font) + 6;

	line[0] = 0;
	while (*s)
	{
		int n = 0;

		if (*s == '\n')
		{
			rg_text(font, x, y, rgb, line);
			y += lh;
			line[0] = 0;
			s++;
			continue;
		}
		while (s[n] && s[n] != ' ' && s[n] != '\n')
			n++;
		snprintf(next, sizeof next, "%s%s%.*s", line, line[0] ? " " : "", n, s);
		if (line[0] && rg_text_w(font, next) > w)
		{
			rg_text(font, x, y, rgb, line);
			y += lh;
			snprintf(line, sizeof line, "%.*s", n, s);
		}
		else
			snprintf(line, sizeof line, "%s", next);
		s += n;
		if (*s == ' ')
			s++;
	}
	if (line[0])
	{
		rg_text(font, x, y, rgb, line);
		y += lh;
	}
	return y;
}

/* The window around the page: the gradient panel with the logo, and the bar the buttons sit on */
static void frame(void)
{
	uint32_t top = rg_colour(RG_DARK_BLUE), bot = rg_colour(RG_LIGHT_BLUE);
	int y, ph = H - BAR_H;

	rg_tile_origin(0, 0);
	rg_marble(0, 0, W, H);
	for (y = 0; y < ph; y++)
	{
		int r = ((top >> 16 & 255) * (ph - y) + (bot >> 16 & 255) * y) / ph;
		int g = ((top >> 8 & 255) * (ph - y) + (bot >> 8 & 255) * y) / ph;
		int b = ((top & 255) * (ph - y) + (bot & 255) * y) / ph;

		rg_fill(0, y, PANEL_W, 1, r << 16 | g << 8 | b);
	}
	rg_fill(PANEL_W, 0, 1, ph, rg_colour(RG_C4));
	rg_image(logo, (PANEL_W - 104) / 2, 26, 104, 104);
	rg_text_centre(rg_font(RG_BOLD, 18 * 16), 0, 142, PANEL_W, 30, rg_colour(RG_WHITE), "PiArcBoot");
	rg_text_centre(RG_DESKTOP, 0, 170, PANEL_W, 22, rg_colour(RG_WHITE), "Version " PIARCBOOT_VERSION);
	rg_fill(0, ph, W, 1, rg_colour(RG_C4));
	rg_fill(0, ph + 1, W, 1, rg_colour(RG_WHITE));
}

/* Back, the default button and Cancel along the bottom, each left out when its label is NULL.
   Returns 1 for Back, 2 for the default button (or Return), 3 for Cancel (or Escape). */
static int buttons(const char *back, const char *next, const char *cancel)
{
	int by = H - BAR_H + 12, cx = W - 18 - BTN_W, nx = cancel ? cx - 26 - BTN_W : cx;

	if (cancel && (rg_button(cx, by, BTN_W, BTN_H, cancel, 1) || rg_in.key == SDLK_ESCAPE))
		return 3;
	if (next && (rg_default_button(nx - 4, by - 4, BTN_W + 8, BTN_H + 8, next, 1) || rg_in.key == SDLK_RETURN))
		return 2;
	if (back && rg_button(nx - 14 - BTN_W, by, BTN_W, BTN_H, back, 1))
		return 1;
	return 0;
}

static const char *shortcut_words(void)
{
	static char s[160];

	if (desktop && menu)
		snprintf(s, sizeof s, ", with a shortcut on the desktop and an entry in the " MENU_NAME);
	else if (desktop)
		snprintf(s, sizeof s, ", with a shortcut on the desktop");
	else if (menu)
		snprintf(s, sizeof s, ", with an entry in the " MENU_NAME);
	else
		s[0] = 0;
	return s;
}

int setup_main(int argc, char **argv, const char *script)
{
	static char field[700];
	char text[1200], error[300] = "";
	const char *shot, *drop;
	int quiet = 0, remove_it = 0, page = P_WELCOME, choice = 0, start_it = 1, quit = 0, frame_no = 0, i, need;
	int x = PANEL_W + 26, w = W - PANEL_W - 52, title;
	uint32_t ink;
	uint8_t *rgba;
	rg_input_t held;

	field[0] = 0;
	for (i = 0; i < argc; i++)
		if (!strcmp(argv[i], "--quiet"))
			quiet = 1;
		else if (!strcmp(argv[i], "--remove"))
			quiet = remove_it = 1;
		else if (!strcmp(argv[i], "--dir") && i + 1 < argc)
			snprintf(field, sizeof field, "%s", argv[++i]);
		else if (!strcmp(argv[i], "--test") && i + 1 < argc)
			i++;            /* main has it */
		else
		{
			fprintf(stderr, "usage: piarcboot setup [--quiet] [--remove] [--dir <folder>]\n"
				"  --quiet          install or update with the usual settings, showing no pages\n"
				"  --remove         remove PiArcBoot and its shortcuts, keeping games and saves\n"
				"  --dir <folder>   install in this folder\n");
			return 2;
		}
	if (script && !exists(script))
	{
		fprintf(stderr, "piarcboot setup: there is no test script %s\n", script);
		return 2;
	}
#ifndef _WIN32
	if (!getenv("DISPLAY") && !getenv("WAYLAND_DISPLAY") && !getenv("SDL_VIDEODRIVER"))
		quiet = 1;              /* over SSH, say */
	if (field[0] == '~' && (field[1] == '/' || !field[1]) && getenv("HOME"))
	{
		snprintf(text, sizeof text, "%s%s", getenv("HOME"), field + 1);
		snprintf(field, sizeof field, "%s", text);
	}
#endif
	snprintf(src, sizeof src, "%s", store_exe());
	src[base_name(src) - src - 1] = 0;
	snprintf(text, sizeof text, "%s" SEP "ROM311", src);
	if (!exists(text))
	{
		fprintf(stderr, "piarcboot setup installs the PiArcBoot folder it is in, and %s has no ROM311.\n", src);
		return 1;
	}
	find_installed();
	need = (int)((tree_size(src, 1) + (1 << 20) - 1) >> 20);

	rgba = malloc(128 * 128 * 4);
	icon_app(rgba);
	if (!quiet && rg_open("Install PiArcBoot", W, H, 0, "piarcboot-setup", rgba))
		quiet = 1;              /* no desktop to show the pages on after all */
	if (quiet)
	{
		char exe[800];

		free(rgba);
		snprintf(exe, sizeof exe, "%s" SEP EXE, field);
		if (field[0] && strcmp(field, found) && exists(exe))
			snprintf(found, sizeof found, "%s", field);     /* the copy in the folder named, not the one found */
		if (remove_it)
		{
			if (!found[0])
			{
				fprintf(stderr, "PiArcBoot isn't installed%s%s.\n", field[0] ? " in " : "", field);
				return 1;
			}
			action = A_REMOVE;
			snprintf(dest, sizeof dest, "%s", found);
			printf("Removing PiArcBoot from %s\n", dest);
		}
		else if (found[0] && (!field[0] || !strcmp(field, found)))
		{
			action = A_UPDATE;
			snprintf(dest, sizeof dest, "%s", found);
			has_shortcuts();
		}
		else if (field[0])
			snprintf(dest, sizeof dest, "%s", field);
		else
			default_dir(dest, sizeof dest);
		if (!remove_it && bad_folder(dest))
		{
			fprintf(stderr, "Can't install in %s: %s\n", dest, bad_folder(dest));
			return 1;
		}
		if (!remove_it)
			printf("%s PiArcBoot %s in %s\n", action == A_UPDATE ? "Updating" : "Installing", PIARCBOOT_VERSION, dest);
		work(NULL);
		if (prog.done != 1)
		{
			fprintf(stderr, "%s\n", prog.error);
			return 1;
		}
		if (remove_it)
			printf("PiArcBoot and its shortcuts are removed. Your games and saves are kept.\n");
		else
			printf("PiArcBoot %s is installed in %s%s.\n", PIARCBOOT_VERSION, dest, shortcut_words());
		return 0;
	}

	logo = rg_image_texture(rgba, 128, 128, 1);
	free(rgba);
	if (!field[0])
		default_dir(field, sizeof field);
	SDL_StartTextInput();
	if (script)
		rg_test_load(script);
	title = rg_font(RG_BOLD, 16 * 16);
	ink = rg_colour(RG_BLACK);
	while (!quit)
	{
		SDL_Event e;
		int y = 26, b, closed = 0;

		shot = drop = NULL;
		rg_begin();
		if (!script)
			SDL_WaitEventTimeout(NULL, page == P_WORKING ? 16 : 250);
		while (SDL_PollEvent(&e))
		{
			rg_event(&e);
			closed |= e.type == SDL_QUIT;
		}
		if (script)
			closed |= rg_test_step(frame_no, &shot, &drop);
		frame_no++;
		held = rg_in;
		if (error[0])           /* the error box takes the clicks and keys, not the page under it */
		{
			rg_in.x = rg_in.y = -1;
			rg_in.key = rg_in.pressed = rg_in.released = 0;
		}
		frame();

		switch (page)
		{
			case P_WELCOME:
			rg_text(title, x, y, ink, "Welcome to PiArcBoot");
			snprintf(text, sizeof text, "This setup installs PiArcBoot %s on your " MACHINE ", with a shortcut on the desktop and an entry in the " MENU_NAME ".\n\n"
				 "PiArcBoot runs Acorn Archimedes games, in a window or full screen.\n\n"
				 "To continue, click Next. To exit without installing, click Cancel.", PIARCBOOT_VERSION);
			wrap(RG_DESKTOP, x, y + 44, w, ink, text);
			b = buttons(NULL, "Next", "Cancel");
			if (b == 2)
				page = found[0] ? P_FOUND : P_FOLDER;
			quit = b == 3;
			break;

			case P_FOUND:
			rg_text(title, x, y, ink, "PiArcBoot Is Already Installed");
			if (found_version[0])
				snprintf(text, sizeof text, "PiArcBoot %s is installed in this folder:", found_version);
			else
				snprintf(text, sizeof text, "PiArcBoot is installed in this folder:");
			y = wrap(RG_DESKTOP, x, y + 44, w, ink, text) + 4;
			rg_display(x, y, w, 28, found);
			y += 44;
			snprintf(text, sizeof text, !strcmp(found_version, PIARCBOOT_VERSION) ? "Reinstall it" :
				 version_cmp(found_version, PIARCBOOT_VERSION) > 0 ? "Replace it with %s, an earlier version" : "Update it to %s", PIARCBOOT_VERSION);
			rg_radio(x, y, 22, text, &choice, 0, 1);
			rg_radio(x, y + 30, 22, "Remove it", &choice, 1, 1);
			wrap(RG_DESKTOP, x, y + 76, w, rg_colour(RG_C4), "Your games and saves are kept either way.");
			b = buttons("Back", "Next", "Cancel");
			if (b == 1)
				page = P_WELCOME;
			else if (b == 2)
			{
				action = choice ? A_REMOVE : A_UPDATE;
				snprintf(dest, sizeof dest, "%s", found);
				has_shortcuts();
				page = P_READY;
			}
			quit = b == 3;
			break;

			case P_FOLDER:
			rg_text(title, x, y, ink, "Select Install Location");
			y += 48;
			rg_field(x, y, w - BTN_W - 12, 28, field, sizeof field, 1);
			if (rg_button(x + w - BTN_W, y + 1, BTN_W, BTN_H, "Change...", 1))
			{
				pick_folder(field, sizeof field);
				rg_in.pressed = rg_in.released = 0;
			}
			{
				long long room = free_space(field);

				if (room >= 0)
					snprintf(text, sizeof text, "Needs %d MB. %.1f GB free.", need, room / 1073741824.0);
				else
					snprintf(text, sizeof text, "Needs %d MB.", need);
				rg_text(RG_DESKTOP, x, y + 38, rg_colour(RG_C4), text);
			}
			rg_option(x, y + 80, 22, "Create desktop shortcut", &desktop, 1);
			rg_option(x, y + 110, 22, "Create menu shortcut", &menu, 1);
			b = buttons("Back", "Next", "Cancel");
			if (b == 1)
				page = P_WELCOME;
			else if (b == 2)
			{
				char *f = field;

				while (*f == ' ')
					f++;
				f[strcspn(f, "\r\n")] = 0;
				while (strlen(f) > 1 && (f[strlen(f) - 1] == ' ' || f[strlen(f) - 1] == SEP[0]))
					f[strlen(f) - 1] = 0;
#ifndef _WIN32
				if (f[0] == '~' && (f[1] == '/' || !f[1]) && getenv("HOME"))
				{
					snprintf(text, sizeof text, "%s%s", getenv("HOME"), f + 1);
					f = text;
				}
#endif
				if (bad_folder(f))
					snprintf(error, sizeof error, "%s", bad_folder(f));
				else
				{
					snprintf(dest, sizeof dest, "%s", f);
					action = A_INSTALL;
					page = P_READY;
				}
			}
			quit = b == 3;
			break;

			case P_READY:
			rg_text(title, x, y, ink, action == A_REMOVE ? "Ready to Remove" : action == A_UPDATE ? "Ready to Update" : "Ready to Install");
			y = wrap(RG_DESKTOP, x, y + 44, w, ink, action == A_REMOVE ? "PiArcBoot is ready to be removed:" : "PiArcBoot is ready to install with these settings:") + 4;
			rg_slab(x, y, w, 96, rg_colour(RG_WHITE), 1);
			{
				char lines[3][800];
				int n = 0;

				if (action == A_INSTALL)
				{
					snprintf(lines[n++], sizeof *lines, "Install PiArcBoot %s in %s", PIARCBOOT_VERSION, dest);
					if (desktop)
						snprintf(lines[n++], sizeof *lines, "Put a shortcut on the desktop");
					if (menu)
						snprintf(lines[n++], sizeof *lines, "Add PiArcBoot to the " MENU_NAME);
				}
				else if (action == A_UPDATE)
				{
					snprintf(lines[n++], sizeof *lines, "%s PiArcBoot %s in %s", !strcmp(found_version, PIARCBOOT_VERSION) ? "Reinstall" :
						 version_cmp(found_version, PIARCBOOT_VERSION) > 0 ? "Go back to" : "Update to", PIARCBOOT_VERSION, dest);
					snprintf(lines[n++], sizeof *lines, "Keep its shortcuts as they are");
					snprintf(lines[n++], sizeof *lines, "Keep your games and saves");
				}
				else
				{
					snprintf(lines[n++], sizeof *lines, "Remove PiArcBoot from %s", dest);
					snprintf(lines[n++], sizeof *lines, "Remove its shortcuts");
					snprintf(lines[n++], sizeof *lines, "Keep your games and saves");
				}
				for (i = 0; i < n; i++)
					rg_text_fit(RG_DESKTOP, x + 12, y + 10 + i * 28, w - 24, ink, lines[i]);
			}
			snprintf(text, sizeof text, "Click %s to begin. To review or change any settings, click Back.",
				 action == A_REMOVE ? "Remove" : action == A_UPDATE ? "Update" : "Install");
			wrap(RG_DESKTOP, x, y + 112, w, ink, text);
			b = buttons("Back", action == A_REMOVE ? "Remove" : action == A_UPDATE ? "Update" : "Install", "Cancel");
			if (b == 1)
				page = action == A_INSTALL ? P_FOLDER : P_FOUND;
			else if (b == 2)
			{
				memset(&prog, 0, sizeof prog);
				cancel = 0;
				page = P_WORKING;
				SDL_DetachThread(SDL_CreateThread(work, "setup", NULL));
			}
			quit = b == 3;
			break;

			case P_WORKING:
			rg_text(title, x, y, ink, action == A_REMOVE ? "Removing PiArcBoot" : action == A_UPDATE ? "Updating PiArcBoot" : "Installing PiArcBoot");
			rg_text_fit(RG_DESKTOP, x, y + 58, w, ink, cancel ? "Cancelling" : prog.step);
			rg_slab(x, y + 90, w, 18, rg_colour(RG_WHITE), 1);
			rg_fill(x + 2, y + 92, (w - 4) * prog.percent / 100, 14, rg_colour(RG_DARK_BLUE));
			if (buttons(NULL, NULL, "Cancel") == 3 || closed)
				cancel = 1;
			closed = 0;
			if (prog.done == 1)
				page = P_DONE;
			else if (prog.done == -2)
				quit = 1;
			else if (prog.done == -1)
			{
				snprintf(error, sizeof error, "%s", prog.error);
				page = P_READY;
			}
			break;

			case P_DONE:
			if (action == A_REMOVE)
			{
				rg_text(title, x, y, ink, "PiArcBoot Removed");
				wrap(RG_DESKTOP, x, y + 44, w, ink, "PiArcBoot and its shortcuts are removed. Your games and saves are kept, so installing PiArcBoot again brings them back.\n\nClick Finish to exit.");
			}
			else
			{
				rg_text(title, x, y, ink, action == A_UPDATE ? "Update Complete" : "Installation Complete");
				snprintf(text, sizeof text, "PiArcBoot %s is installed%s.\n\nClick Finish to exit.", PIARCBOOT_VERSION, shortcut_words());
				y = wrap(RG_DESKTOP, x, y + 44, w, ink, text) + 14;
				rg_option(x, y, 22, "Start PiArcBoot", &start_it, 1);
			}
			if (buttons(NULL, "Finish", NULL) == 2)
			{
				if (start_it && action != A_REMOVE)
					start_installed();
				quit = 1;
			}
			break;
		}
		if (error[0])
		{
			rg_in = held;
			if (rg_error_box_with((W - RG_ERROR_W) / 2, (H - RG_ERROR_H + 20) / 2, "Install PiArcBoot", error, RG_ERROR, "OK", NULL) ||
			    rg_in.key == SDLK_RETURN || rg_in.key == SDLK_ESCAPE)
				error[0] = 0;
		}
		if (closed && page != P_WORKING)
			quit = 1;
		if (shot)
			rg_save_shot(shot);
		rg_present();
	}
	while (page == P_WORKING && !prog.done)        /* closed while it worked: it stops first */
		SDL_Delay(10);
	rg_close();
	return 0;
}
