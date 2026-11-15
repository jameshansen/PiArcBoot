/* PiArcBoot: a game window's menu bar, as the system the game is running on makes them.

   On Windows it is the window's own menu. On Linux it is a GTK menu bar, in the desktop's theme,
   set into the top of the window. GTK is loaded when the game starts and not needed to build,
   and it draws through X11, which is also what SDL uses on a Raspberry Pi. Where neither can be
   had, menubar_open returns 0 and the window has no menus, though its keys still work.

   The menus are given as a flat list of items, an item in a submenu naming the item that opens
   it, and a chosen item comes back as its command number from menubar_poll. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL.h>
#include <SDL_syswm.h>
#include "piarcboot.h"

#define MAX_MENUS 4

static char titles[MAX_MENUS][32];
static int n_titles, chosen;

static void keep_titles(const char *const *names, int n)
{
	for (n_titles = 0; n_titles < n && n_titles < MAX_MENUS; n_titles++)
		snprintf(titles[n_titles], sizeof titles[0], "%s", names[n_titles]);
}

#if defined(_WIN32)

static HWND hwnd;
static HMENU bar;
static int shown = 1;

int menubar_open(const char *const *names, int n)
{
	SDL_SysWMinfo info;

	SDL_VERSION(&info.version);
	if (!SDL_GetWindowWMInfo(rg_win, &info) || info.subsystem != SDL_SYSWM_WINDOWS)
		return 0;
	hwnd = info.info.win.window;
	SDL_EventState(SDL_SYSWMEVENT, SDL_ENABLE);     /* a chosen item arrives as a WM_COMMAND message */
	keep_titles(names, n);
	return 1;
}

void menubar_set(const menu_item_t *items, int n)
{
	HMENU old = bar, menu[MAX_MENUS], *sub = calloc(n ? n : 1, sizeof *sub);
	int i;

	bar = CreateMenu();
	for (i = 0; i < n_titles; i++)
	{
		menu[i] = CreatePopupMenu();
		AppendMenuA(bar, MF_POPUP, (UINT_PTR)menu[i], titles[i]);
	}
	for (i = 0; i < n; i++)         /* the submenus first, so that each is there when its item is made */
		if (items[i].sub > 0 && items[i].sub <= i && !sub[items[i].sub - 1])
			sub[items[i].sub - 1] = CreatePopupMenu();
	for (i = 0; i < n; i++)
	{
		HMENU in = items[i].sub > 0 && items[i].sub <= i ? sub[items[i].sub - 1] : menu[items[i].menu];
		UINT flags = MF_STRING | (items[i].checked ? MF_CHECKED : 0) | (items[i].shaded ? MF_GRAYED : 0);

		if (items[i].menu < 0 || items[i].menu >= n_titles)
			continue;
		if (items[i].rule)
			AppendMenuA(in, MF_SEPARATOR, 0, NULL);
		if (sub[i])
			AppendMenuA(in, flags | MF_POPUP, (UINT_PTR)sub[i], items[i].label);
		else
			AppendMenuA(in, flags, items[i].cmd, items[i].label);
	}
	free(sub);
	if (shown)
	{
		SetMenu(hwnd, bar);
		DrawMenuBar(hwnd);
	}
	if (old)
		DestroyMenu(old);
}

void menubar_event(const SDL_Event *e)
{
	if (e->type == SDL_SYSWMEVENT && e->syswm.msg->msg.win.msg == WM_COMMAND && !HIWORD(e->syswm.msg->msg.win.wParam))
		chosen = LOWORD(e->syswm.msg->msg.win.wParam);
}

int menubar_poll(void)
{
	int cmd = chosen;

	chosen = 0;
	return cmd;
}

void menubar_show(int on)
{
	shown = on;
	SetMenu(hwnd, on ? bar : NULL);
}

/* The menu is outside the part of the window that is drawn in */
int menubar_height(void)
{
	return 0;
}

void menubar_width(int w)
{
	(void)w;
}

#elif defined(__linux__) && defined(SDL_VIDEO_DRIVER_X11)

#include <dlfcn.h>

typedef void *obj;      /* a GtkWidget, a GdkWindow or a GdkDisplay: only ever handed back to GTK */

/* The GTK 3 calls used, looked up by name when the game starts */
static struct
{
	void (*set_allowed_backends)(const char *);
	int (*init_check)(int *, char ***);
	obj (*window_new)(int);
	obj (*menu_bar_new)(void);
	obj (*menu_new)(void);
	obj (*menu_item_new_with_label)(const char *);
	obj (*check_menu_item_new_with_label)(const char *);
	void (*check_menu_item_set_active)(obj, int);
	obj (*separator_menu_item_new)(void);
	void (*menu_shell_append)(obj, obj);
	void (*menu_item_set_submenu)(obj, obj);
	void (*container_add)(obj, obj);
	void (*widget_show_all)(obj);
	void (*widget_show)(obj);
	void (*widget_hide)(obj);
	void (*widget_destroy)(obj);
	void (*widget_realize)(obj);
	void (*widget_get_preferred_height)(obj, int *, int *);
	int (*widget_get_scale_factor)(obj);
	obj (*widget_get_window)(obj);
	obj (*widget_get_display)(obj);
	void (*window_resize)(obj, int, int);
	obj (*foreign_new)(obj, unsigned long);
	void (*window_reparent)(obj, obj, int, int);
	unsigned long (*signal_connect)(obj, const char *, void (*)(void), void *, void *, int);
	int (*events_pending)(void);
	int (*main_iteration_do)(int);
	void (*menu_item_set_label)(obj, const char *);
	void (*widget_set_sensitive)(obj, int);
	int (*check_menu_item_get_active)(obj);
	int (*window_get_events)(obj);
	void (*window_set_events)(obj, int);
} gtk;

#define KEY_MASKS (1 << 10 | 1 << 11)  /* GDK_KEY_PRESS_MASK and GDK_KEY_RELEASE_MASK */

static const char *const gtk_names[] =
{
	"gdk_set_allowed_backends", "gtk_init_check", "gtk_window_new", "gtk_menu_bar_new", "gtk_menu_new",
	"gtk_menu_item_new_with_label", "gtk_check_menu_item_new_with_label", "gtk_check_menu_item_set_active",
	"gtk_separator_menu_item_new", "gtk_menu_shell_append", "gtk_menu_item_set_submenu", "gtk_container_add",
	"gtk_widget_show_all", "gtk_widget_show", "gtk_widget_hide", "gtk_widget_destroy", "gtk_widget_realize",
	"gtk_widget_get_preferred_height", "gtk_widget_get_scale_factor", "gtk_widget_get_window", "gtk_widget_get_display",
	"gtk_window_resize", "gdk_x11_window_foreign_new_for_display", "gdk_window_reparent", "g_signal_connect_data",
	"gtk_events_pending", "gtk_main_iteration_do", "gtk_menu_item_set_label", "gtk_widget_set_sensitive",
	"gtk_check_menu_item_get_active", "gdk_window_get_events", "gdk_window_set_events"
};

static obj strip, bar;          /* a borderless GTK window set into the top of the game's, and the menu bar in it */
static obj *made;               /* each item's widget, as last made */
static menu_item_t *shown_items;        /* and what they were made from */
static int n_made;
static int factor = 1, height, width, building, hidden;

static void on_choose(obj item, void *cmd)
{
	(void)item;
	if (!building)          /* ticking an item as it is made counts as choosing it, to GTK */
		chosen = (int)(intptr_t)cmd;
}

/* GTK activates an item as its submenu opens, which unticks a ticked one: it is ticked again */
static void keep_ticked(obj item, void *unused)
{
	(void)unused;
	if (!gtk.check_menu_item_get_active(item))
		gtk.check_menu_item_set_active(item, 1);
}

int menubar_open(const char *const *names, int n)
{
	SDL_SysWMinfo info;
	void *lib, **slot = (void **)&gtk;
	obj parent;
	size_t i;

	SDL_VERSION(&info.version);
	if (!SDL_GetWindowWMInfo(rg_win, &info) || info.subsystem != SDL_SYSWM_X11)
		return 0;
	lib = dlopen("libgtk-3.so.0", RTLD_NOW | RTLD_GLOBAL);
	for (i = 0; i < sizeof gtk_names / sizeof *gtk_names; i++)
		if (!lib || !(slot[i] = dlsym(lib, gtk_names[i])))
			return 0;
	gtk.set_allowed_backends("x11");
	if (!gtk.init_check(NULL, NULL))
		return 0;
	strip = gtk.window_new(1);              /* GTK_WINDOW_POPUP: one the window manager leaves alone */
	gtk.widget_realize(strip);
	parent = gtk.foreign_new(gtk.widget_get_display(strip), info.info.x11.window);
	if (!parent)
		return 0;
	gtk.window_reparent(gtk.widget_get_window(strip), parent, 0, 0);
	/* The strip is a window inside the game's, and X gives the keys to the window the pointer is
	   over when that one asks for them. It doesn't ask, so they reach the game's window wherever
	   the pointer is. */
	gtk.window_set_events(gtk.widget_get_window(strip), gtk.window_get_events(gtk.widget_get_window(strip)) & ~KEY_MASKS);
	factor = gtk.widget_get_scale_factor(strip);
	if (factor < 1)
		factor = 1;
	keep_titles(names, n);
	return 1;
}

/* Whether two items differ in more than their labels */
static int unlike(const menu_item_t *a, const menu_item_t *b)
{
	return a->menu != b->menu || a->cmd != b->cmd || a->checked != b->checked || a->rule != b->rule ||
	       a->shaded != b->shaded || a->sub != b->sub;
}

void menubar_set(const menu_item_t *items, int n)
{
	obj menu[MAX_MENUS], item, *sub;
	int i, least;

	/* Only the words changed: they are changed in place, so that a menu open just now stays open */
	for (i = 0; bar && n == n_made && i < n && !unlike(&items[i], &shown_items[i]); i++);
	if (bar && n == n_made && i == n)
	{
		for (i = 0; i < n; i++)
			if (made[i] && strcmp(items[i].label, shown_items[i].label))
				gtk.menu_item_set_label(made[i], items[i].label);
		memcpy(shown_items, items, n * sizeof *items);
		return;
	}
	building = 1;
	if (bar)
		gtk.widget_destroy(bar);
	free(made);
	free(shown_items);
	made = calloc(n ? n : 1, sizeof *made);
	sub = calloc(n ? n : 1, sizeof *sub);
	shown_items = malloc((n ? n : 1) * sizeof *items);
	memcpy(shown_items, items, n * sizeof *items);
	n_made = n;
	bar = gtk.menu_bar_new();
	for (i = 0; i < n_titles; i++)
	{
		item = gtk.menu_item_new_with_label(titles[i]);
		menu[i] = gtk.menu_new();
		gtk.menu_item_set_submenu(item, menu[i]);
		gtk.menu_shell_append(bar, item);
	}
	for (i = 0; i < n; i++)
	{
		int p = items[i].sub - 1;       /* the item whose submenu it is in, which comes before it */
		obj in;

		if (items[i].menu < 0 || items[i].menu >= n_titles)
			continue;
		if (p >= 0 && p < i && made[p])
		{
			if (!sub[p])
			{
				sub[p] = gtk.menu_new();
				gtk.menu_item_set_submenu(made[p], sub[p]);
			}
			in = sub[p];
		}
		else
			in = menu[items[i].menu];
		if (items[i].rule)
			gtk.menu_shell_append(in, gtk.separator_menu_item_new());
		if (items[i].checked)
		{
			item = gtk.check_menu_item_new_with_label(items[i].label);
			gtk.check_menu_item_set_active(item, 1);
		}
		else
			item = gtk.menu_item_new_with_label(items[i].label);
		if (items[i].cmd)
			gtk.signal_connect(item, "activate", (void (*)(void))on_choose, (void *)(intptr_t)items[i].cmd, NULL, 0);
		else if (items[i].checked)      /* a ticked item that opens a submenu */
			gtk.signal_connect(item, "toggled", (void (*)(void))keep_ticked, NULL, NULL, 0);
		if (items[i].shaded)
			gtk.widget_set_sensitive(item, 0);
		gtk.menu_shell_append(in, item);
		made[i] = item;
	}
	free(sub);
	gtk.container_add(strip, bar);
	gtk.widget_show_all(hidden ? bar : strip);     /* a strip hidden for full screen stays hidden */
	gtk.widget_get_preferred_height(bar, &least, &height);
	if (width)
		gtk.window_resize(strip, width, height);
	building = 0;
}

void menubar_event(const SDL_Event *e)
{
	(void)e;
}

int menubar_poll(void)
{
	int cmd;

	while (gtk.events_pending())
		gtk.main_iteration_do(0);
	cmd = chosen;
	chosen = 0;
	return cmd;
}

void menubar_show(int on)
{
	hidden = !on;
	(on ? gtk.widget_show : gtk.widget_hide)(strip);
}

/* How much of the top of the window it covers, in real pixels */
int menubar_height(void)
{
	return height * factor;
}

/* Keeps it as wide as the window, given in real pixels */
void menubar_width(int w)
{
	if (w / factor != width && height)
	{
		width = w / factor;
		gtk.window_resize(strip, width, height);
	}
}

#else

int menubar_open(const char *const *names, int n)
{
	(void)names;
	(void)n;
	(void)keep_titles;
	return 0;
}

void menubar_set(const menu_item_t *items, int n) { (void)items; (void)n; }
void menubar_event(const SDL_Event *e) { (void)e; }
int menubar_poll(void) { return chosen; }
void menubar_show(int on) { (void)on; }
int menubar_height(void) { return 0; }
void menubar_width(int w) { (void)w; }

#endif
