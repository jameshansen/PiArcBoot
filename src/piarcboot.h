/* PiArcBoot: what the source files share */
#ifndef PIARCBOOT_H
#define PIARCBOOT_H

#ifndef PIARCBOOT_VERSION
#define PIARCBOOT_VERSION "dev"        /* the build gives the release's own, from its tag */
#endif

#include <stddef.h>
#include <stdint.h>

/* emu.c, screen.c, wd1772.c: the emulated machine, ArcEm's with PiArcBoot's layer over it */
#define EMU_FB_W 1600           /* width of a row of emu_fb, in pixels */

extern uint32_t *emu_fb;        /* the picture, &00RRGGBB, emu_w by emu_h in rows of EMU_FB_W */
extern int emu_w, emu_h;
extern int emu_dbl;             /* each line of the picture is to be shown twice */
extern unsigned emu_frames;     /* pictures delivered so far */
extern int emu_mute;
extern uint8_t emu_host_keys[512];      /* keys held on the real keyboard, by SDL scancode */

extern const char *emu_extrom_dir;     /* a folder of modules for an extension ROM, read at power on */
extern const char *emu_hostfs_dir;     /* the folder HostFS shows the machine */
int emu_start(const char *rom, const uint8_t *cmos, int fdc);
void emu_step(void);
void emu_audio_open(void);
void emu_audio_fade_in(int ms);                 /* the sound from here rises from nothing to full over that long */
void emu_mouse(int dx, int dy, int buttons);
void emu_script(const char *text);
void emu_script_line(const char *line);
int emu_script_busy(void);
void emu_script_stop(void);
void emu_save_shot(const char *fn);
void emu_state(char *buf, size_t size);         /* a line that tells the end of two runs apart */

/* savestate.c: the whole machine kept in a file, and put back from one */
int state_ready(void);                          /* it can be taken or put back now: no floppy command is going */
int state_save(const char *path, int wait, const uint32_t *shot, int sw, int sh);       /* with a screenshot, written on a thread of its own unless wait is set; -1 if it can't be now */
uint32_t *state_shot(const char *path, int *w, int *h); /* a state's screenshot, &00RRGGBB pixels, or NULL */
uint8_t *state_take(size_t *len);                       /* the machine now, held in memory; NULL if it can't be now */
int state_put(const uint8_t *parts, size_t len);        /* and back to it: -1, and the machine as it was, if it can't be */
int state_writing(void);                        /* a state is still being written */
int state_write_failed(void);                   /* the last one written on its own thread didn't reach its file */
int state_load(const char *path, char *why, size_t size);       /* -1, the machine as it was and why in why, if it can't be */

/* ROO311's HostLink module, which works the desktop for the game window */
extern int emu_hostlink;        /* it has made itself known */
extern int emu_host_modes;      /* screen mode changes it has reported */
extern int emu_host_errors;     /* error boxes it has seen open */
extern int emu_host_error_open; /* one is open just now */
extern int emu_host_tasks;      /* tasks it has seen start in the desktop */
extern int emu_host_modes_at_run, emu_host_errors_at_run, emu_host_tasks_at_run;      /* those counts when it last took up a command to start, or -1 */
extern int emu_host_vidc_at_run;        /* and machine_mode_sets then */
extern int emu_host_clicked, emu_host_missed;  /* icon bar clicks made, and asked for with no such icon */
extern int emu_host_nwins, emu_host_wins[31][4];        /* the windows on the screen as it last said: x0, y0, x1, y1 in OS units */
extern int emu_host_xeig, emu_host_yeig;        /* the screen mode's eigen factors, as it last said, or -1 */
void machine_os_per_pixel(int *x2, int *y2);    /* half OS units to a pixel of the picture, across and down */
void emu_host_run(const char *command);        /* start a *command as a task in the desktop */
void emu_host_click(const char *sprite);       /* click the icon bar icon with that sprite on it */
void emu_host_forget(void);                     /* drop what hasn't been sent yet */
void emu_host_pointer(int x, int y);           /* put the pointer there, in OS units */

extern int machine_fixed_time;  /* set before power on: the clock starts at noon on 1 January 1992 and runs with machine time */
extern int machine_disc_reads;  /* sector reads the floppy controller has been asked for */
extern int machine_mode_sets;   /* writes to the VIDC's horizontal cycle register, as setting up a screen mode makes */
int machine_disc(int drive, const char *path, int write_protect);
const char *machine_disc_path(int drive);      /* the image in that drive, or NULL */
void machine_cmos_desktop(uint8_t *cmos, int mode, int monitor);     /* a desktop mode and monitor type in place of Auto */
int machine_motor_on(void);     /* the floppy drive's motor is turning */
void machine_close(void);       /* writes back what games saved to their discs */

/* disc.c: disc images */
uint8_t *jfd_read(const char *path, size_t *len);       /* the file, unpacked if it is gzip compressed */
uint8_t *jfd_flatten(const char *path, size_t *len);    /* a JFD as a flat image of its sectors */
uint8_t *disc_sectors(const char *path, size_t *len);   /* any disc image as ADFS sees its sectors, each where ADFS looks for it */
int disc_selftest(void);

/* adfs.c: ADFS D format floppy images made from files */
typedef struct
{
	const char *path;       /* RISC OS's, with . between the parts */
	uint32_t load, exec, attr;
	const uint8_t *data;
	uint32_t len;
	int dir;
} adfs_file_t;

uint8_t *adfs_make(const adfs_file_t *files, int n, const char *disc_name, size_t *len, char *error, size_t error_size);
int adfs_from_zip(const uint8_t *zip, size_t len, const char *prefix, adfs_file_t **out);
void adfs_free(adfs_file_t *files, int n);
int adfs_selftest(void);
int adfs_app_root(adfs_file_t *files, int n);   /* the folder an archive's applications are in made the top */
int adfs_read(const uint8_t *img, size_t len, adfs_file_t **out, char *disc_name, int *boot_option);   /* the files on an L, D or E format disc */

/* arc.c: Spark and ArcFS archives */
int arc_is_archive(const uint8_t *data, size_t len);
int arc_files(const uint8_t *data, size_t len, adfs_file_t **out, char *error, size_t error_size);
int arc_selftest(void);

/* png.c */
int png_write(const char *fn, const uint8_t *rgba, int w, int h);
uint8_t *png_read(const char *fn, int *w, int *h);

/* util.c */
typedef struct
{
	char name[256];
	uint32_t csize, usize, offset;
	int method;
	int riscos;             /* it carries RISC OS's load and execution addresses and attributes */
	uint32_t load, exec, attr;
} zipent_t;

uint8_t *file_read(const char *fn, size_t *len);
int file_write(const char *fn, const void *data, size_t len);
int zip_list(const uint8_t *z, size_t len, zipent_t **out);
uint8_t *zip_get(const uint8_t *z, size_t len, const zipent_t *e, size_t *n);
int run_command(const char *fmt, ...);
char *run_output(const char *fmt, ...);
int make_dirs(const char *path);
int is_dir(const char *path);
void remove_tree(const char *path);
int copy_tree(const char *from, const char *to);
const char *base_name(const char *path);

/* icon.c */
typedef struct
{
	char name[13];
	int w, h, masked;
	uint8_t *rgba;
} sprite_t;

int sprites_parse(const uint8_t *d, size_t len, sprite_t **out);
void sprites_free(sprite_t *list, int n);
int sprites_scan(const uint8_t *img, size_t len, sprite_t **out);
int sprites_main(const sprite_t *list, int n);
void icon_render(uint8_t *out, const sprite_t *s);
void icon_generic(uint8_t *out);
void icon_app(uint8_t *out);

/* store.c: where games live and what is known about them */
#define MAX_DISCS 9

typedef struct
{
	char id[80];            /* its folder under games/, and for a JASPP game its package name */
	char title[128];
	char info[128];
	char boot[64];          /* the application to run, or "" for a disc that boots with Shift held */
	char version[32];       /* a JASPP package's version */
	char sprite[16];        /* the sprite its icon is made from, when one was picked by hand */
	char preset[80];        /* the presets it takes when they aren't named after its id, for one added by hand that its first disc matched */
	int fdc;                /* 1 for the 82C711 floppy controller */
	int jaspp;              /* 1 from the JASPP list, 0 one of the user's own */
	int classic;            /* 1 from the Classics tab */
	int apps;               /* 1 made from an archive of applications: it starts as a Classics game does */
	int scale;              /* window size, 1 to 3 */
	int fullscreen;
	int crop;               /* 1 shows just the windows on the screen, 0 all of it, -1 as its presets say */
	int autosave;           /* the save slot written every 30 seconds of play, 1 to 3, or 0 for none */
	int nocapture;          /* the mouse isn't captured: the pointer goes where the host's is */
	int win_w, win_h;       /* the game window's size as the player last dragged it, in pixels, or 0 */
	int desktop, menu;      /* has a desktop shortcut, a menu entry */
	int ndiscs;
	char disc[MAX_DISCS][40];       /* the disc image files in its folder */
	uint8_t ro[MAX_DISCS];          /* that disc goes in write protected */
} game_t;

/* A game in the JASPP list */
typedef struct
{
	char id[80], title[128], info[128], boot[64], version[32], url[256];
	unsigned game, size;
	int fdc, wp;
	int discs, set;         /* floppy images in its package, and in the whole game */
	int release;            /* floppy images in its release folder on JASPP's server */
	int floppies;           /* floppy images there are to install it from; with none it isn't listed */
	int tested;             /* 1 for a game from the built-in table, 0 for one JASPP has added since */
} entry_t;

typedef struct
{
	volatile int percent;   /* 0 to 100, or -1 when there is no telling */
	volatile int done;      /* 1 finished, -1 failed */
	volatile int cancel;    /* set by the window to stop it: the install ends as soon as it can, as failed */
	char step[160];
	char error[240];
} progress_t;

const char *store_dir(void);
const char *store_home_dir(void);      /* the data folder of a copy that isn't portable, whatever this one is */
const char *store_exe(void);
char *store_asset(const char *name, char *buf, size_t size);
void store_game_path(const char *id, const char *file, char *buf, size_t size);
int store_load(const char *id, game_t *g);
int store_disc_changed(const game_t *g, int n);
int disc_clue(const char *name, int *at);      /* the disc a file's name says it is, and where that is said */
int store_save(const game_t *g);
int store_list(game_t **out);
void store_remove(const game_t *g);
void store_shortcuts(const game_t *g);
void store_pictures_dir(char *buf, size_t size);       /* the folder Save Snapshot saves in */
void store_shortcut_dirs(char *apps, size_t apps_size, char *desk, size_t desk_size);  /* where menu entries and desktop shortcuts go, on Linux */
void store_icon(const game_t *g, uint8_t *out);
int store_sprites(const game_t *g, sprite_t **out);
int store_set_icon(const game_t *g, const sprite_t *s);
void store_pick_icon(game_t *g);        /* the sprite named after its application, or the most icon-like one */
int store_boot_choices(const game_t *g, char choices[][64], int max);
int catalog_count(void);
const entry_t *catalog_entry(int i);
void catalog_load_feed(const char *text);
int feed_refresh(void);
int feed_check_new(void);
int install_jaspp(const entry_t *e, int desktop, int menu, progress_t *p);
int import_game(char **files, int n, const char *add_to, char *id_out, char *error, size_t error_size);
int spawn_game(const char *id);
int store_fetch(const char *url, const char *dest);     /* downloads a file; 0 when it arrived */

/* setup.c: the installer, "piarcboot setup" */
int setup_main(int argc, char **argv, const char *script);

/* classics.c: free games that come as RISC OS applications, made into floppy images */
enum { CLASSIC_ARCADE, CLASSIC_PACKMAN, CLASSIC_SECTIONS };
typedef struct
{
	char id[80], title[128], info[160];
	char mirror[200];       /* its copy under distributable-games/ in PiArcBoot's repository, or "" */
	char url[400];          /* where its publisher put it */
	char prefix[128];       /* the folder in the zip that holds what goes on the disc */
	char boot[40];          /* the application that starts the game */
	int section;
} classic_t;

/* A setting from presets/<game>.ini beside the program, which presets/README.md in the repository
   describes. 1 with its value in buf if the game's file has the key. */
int preset_setting(const char *id, const char *key, char *buf, size_t size);
const char *needs_support(const adfs_file_t *files, int n);     /* the support pack their !Run files ask for, or "" */
int write_hostfs(const char *dir, const adfs_file_t *f, int n);  /* files written in a folder as HostFS names them; 0 when all were */
int has_installer(const game_t *g, const char *id);             /* its presets have an installer script for it */
int run_installer(game_t *g, const char *id, progress_t *p);    /* its files copied from its discs onto its HostFS folder, kept in installed/ */
void deploy_installed(const char *id);

int classic_count(void);
const classic_t *classic_entry(int i);
void classics_load(void);                       /* the PackMan games from the indexes last fetched */
int classics_refresh(void);                     /* fetches the PackMan indexes; 1 if any arrived */
int classics_sources(char urls[][300], int max);
void classics_add_source(const char *url);
void classics_remove_source(int i);
int install_classic(const classic_t *c, int desktop, int menu, progress_t *p);
int layout_files(game_t *g, const char *dir, adfs_file_t *files, int n, const char *boot, const char *support, progress_t *p);
char **pick_files(int *n);
void open_url(const char *url);

/* The windows are drawn with ROOGUI, which gives them the look of the RISC OS desktop */
#include "roogui.h"
#define SMALL rg_font(RG_MEDIUM, 9 * 16)        /* Homerton at 9 point, for the lines under a game's name */

/* menubar.c: a menu bar of the system's own on the game window */
typedef struct
{
	int menu;               /* which menu it is in, counting from 0 */
	int cmd;                /* what choosing it comes back as */
	int checked;            /* ticked */
	int rule;               /* a line above it */
	int shaded;             /* greyed out, and choosing it does nothing */
	int sub;                /* in the submenu of the item at this place in the list, counting from 1, or 0 */
	char label[64];
} menu_item_t;

union SDL_Event;
int menubar_open(const char *const *titles, int n);
void menubar_set(const menu_item_t *items, int n);
void menubar_event(const union SDL_Event *e);
int menubar_poll(void);
void menubar_show(int on);
int menubar_height(void);
void menubar_width(int w);

/* game.c, launcher.c */
int game_main(const char *id, const char *test_script);
int launcher_main(const char *test_script);

#endif
