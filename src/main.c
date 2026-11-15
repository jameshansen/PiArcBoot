/* PiArcBoot: Acorn Archimedes games, booted straight from their disc images.

     piarcboot                      the launcher
     piarcboot play <game>          start an installed game in a window of its own

   and for building and testing it:

     piarcboot test <rom> <script> [fdc [blank]] run a script against the machine, with no window
     piarcboot play <game> --test <script>       a game window driven by a script, as fast as it goes
     piarcboot launcher --test <script>          the launcher driven by a script
     piarcboot mkicons <packages folder> <out>   the JASPP games' icons, from their packages
     piarcboot icon <file> <out.png> [sprite]    an icon from a sprite file, a disc image or a PNG
     piarcboot list                              the game table
     piarcboot checknew                          note which games new to the feed have floppy images
     piarcboot classic [<game>]                  install a game from the Classics list, or list them
     piarcboot install <game>                    install a game from the JASPP list
     piarcboot add <files>                       add a game from disc images or archives
     piarcboot ls <image>                        the files on a disc image
     piarcboot extract <file> <folder>           the files in an archive or a disc image, named as HostFS names them
     piarcboot check                             the self-tests */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL.h>
#include <zlib.h>
#include "piarcboot.h"
#include "default_cmos.h"

/* Runs a script against the machine with no window. The clock is fixed, so a run repeats exactly.
   The CMOS RAM starts as it does for a game, or as all zeros with "blank" after the fdc number.
   PIARCBOOT_EXTROM and PIARCBOOT_HOSTFS name an extension ROM's folder of modules and HostFS's
   folder, as a game's support pack gives them, and PIARCBOOT_MODE a screen mode for the desktop
   on a multisync monitor, as a game's presets can. */
static int cmd_test(int argc, char **argv)
{
	char *text, state[160];
	uint8_t cmos[256];

	if (argc < 2)
		return 2;
	machine_fixed_time = 1;
	emu_extrom_dir = getenv("PIARCBOOT_EXTROM");
	emu_hostfs_dir = getenv("PIARCBOOT_HOSTFS");
	memcpy(cmos, default_cmos, sizeof cmos);
	if (getenv("PIARCBOOT_MODE"))
		machine_cmos_desktop(cmos, atoi(getenv("PIARCBOOT_MODE")), 1);
	if (emu_start(argv[0], argc > 3 && !strcmp(argv[3], "blank") ? NULL : cmos, argc > 2 ? atoi(argv[2]) : 0))
	{
		fprintf(stderr, "can't read the ROM %s\n", argv[0]);
		return 1;
	}
	text = (char *)file_read(argv[1], NULL);
	if (!text)
	{
		perror(argv[1]);
		return 1;
	}
	emu_script(text);
	while (emu_script_busy())
		emu_step();
	/* What the machine ended up as, to tell two runs apart: its memory, its registers and its clock */
	emu_state(state, sizeof state);
	printf("%s\n", state);
	machine_close();
	return 0;
}

static int same_name(const char *a, const char *b)
{
	for (; *a && tolower((unsigned char)*a) == tolower((unsigned char)*b); a++, b++);
	return !*a && !*b;
}

/* The sprite that stands for an application, from the sprite files in a package: the one named
   after the application, from the file of square-pixel sprites if there is one */
static int package_icon(const uint8_t *zip, size_t len, const char *app, uint8_t *rgba)
{
	static const char *const files[] = { "!Sprites22", "!Sprites" };
	zipent_t *list;
	int n = zip_list(zip, len, &list), f, i, k, done = 0;

	for (f = 0; f < 2 && !done; f++)
		for (i = 0; i < n && !done; i++)
		{
			const char *name = list[i].name, *base = base_name(name);
			sprite_t *sprites;
			uint8_t *data;
			size_t size;
			int count, pick = -1;

			if (!same_name(base, files[f]))
				continue;
			if (app[0] && (base - name < (int)strlen(app) + 1 || strncmp(base - strlen(app) - 1, app, strlen(app))))
				continue;               /* another application's sprites */
			if (!(data = zip_get(zip, len, &list[i], &size)))
				continue;
			count = sprites_parse(data, size, &sprites);
			for (k = 0; k < count; k++)
				if (app[0] && same_name(sprites[k].name, app))
					pick = k;
			if (pick < 0 && count)
				pick = sprites_main(sprites, count);
			if (pick >= 0)
			{
				icon_render(rgba, &sprites[pick]);
				done = 1;
			}
			sprites_free(sprites, count);
			free(data);
		}
	free(list);
	return done;
}

/* The application folder a package holds, as Apps/Games/<folder>: the one the game is started
   from if it is there, or the first */
static void package_app(const uint8_t *zip, size_t len, const char *boot, char *app, size_t size)
{
	zipent_t *list;
	int n = zip_list(zip, len, &list), i;

	app[0] = 0;
	for (i = 0; i < n; i++)
	{
		char dir[256];

		if (sscanf(list[i].name, "Apps/Games/%255[^/]/", dir) != 1 || dir[0] != '!')
			continue;
		if (!app[0] || same_name(dir, boot))
			snprintf(app, size, "%s", dir);
	}
	free(list);
}

static int cmd_mkicons(int argc, char **argv)
{
	uint8_t *rgba = malloc(128 * 128 * 4);
	char path[1024], app[256];
	int i, made = 0, plain = 0;

	if (argc < 2)
		return 2;
	make_dirs(argv[1]);
	for (i = 0; i < catalog_count(); i++)
	{
		const entry_t *e = catalog_entry(i);
		size_t len;
		uint8_t *zip;

		snprintf(path, sizeof path, "%s/%s_%s.zip", argv[0], e->id, e->version);
		zip = file_read(path, &len);
		if (!zip)
		{
			fprintf(stderr, "no package for %s (%s)\n", e->id, path);
			continue;
		}
		package_app(zip, len, e->boot, app, sizeof app);
		if (!package_icon(zip, len, app, rgba) && !package_icon(zip, len, "", rgba))
		{
			fprintf(stderr, "%s: no sprite, so it gets the plain disc\n", e->id);
			icon_generic(rgba);
			plain++;
		}
		free(zip);
		snprintf(path, sizeof path, "%s/%s.png", argv[1], e->id);
		made += !png_write(path, rgba, 128, 128);
	}
	printf("%d icons written, %d of them the plain disc\n", made, plain);
	free(rgba);
	return 0;
}

static int cmd_icon(int argc, char **argv)
{
	uint8_t *rgba = malloc(128 * 128 * 4), *d;
	sprite_t *sprites;
	size_t len;
	int n, i, pick;

	if (argc < 2)
		return 2;
	if (!strcmp(argv[0], "app"))
		icon_app(rgba);
	else if (!strcmp(argv[0], "generic"))
		icon_generic(rgba);
	else if (strlen(argv[0]) > 4 && !strcmp(argv[0] + strlen(argv[0]) - 4, ".png"))
	{
		/* a picture made into an icon as a sprite is, one with see-through parts as a masked sprite */
		sprite_t s;

		memset(&s, 0, sizeof s);
		if (!(s.rgba = png_read(argv[0], &s.w, &s.h)))
		{
			fprintf(stderr, "%s isn't a PNG PiArcBoot reads (8-bit RGB or RGBA)\n", argv[0]);
			return 1;
		}
		for (i = 0; i < s.w * s.h && !s.masked; i++)
			s.masked = s.rgba[i * 4 + 3] < 255;
		icon_render(rgba, &s);
		free(s.rgba);
	}
	else
	{
		d = jfd_flatten(argv[0], &len);
		if (!d)
		{
			perror(argv[0]);
			return 1;
		}
		n = sprites_parse(d, len, &sprites);
		if (!n)
			n = sprites_scan(d, len, &sprites);
		pick = n ? sprites_main(sprites, n) : -1;
		for (i = 0; i < n; i++)
		{
			printf("%-12s %3d x %-3d%s\n", sprites[i].name, sprites[i].w, sprites[i].h, sprites[i].masked ? " masked" : "");
			if (argc > 2 && same_name(sprites[i].name, argv[2]))
				pick = i;
		}
		if (pick < 0)
			icon_generic(rgba);
		else
			icon_render(rgba, &sprites[pick]);
	}
	return png_write(argv[1], rgba, 128, 128) ? 1 : 0;
}

/* Lists a Spark or ArcFS archive: each file's load and execution addresses, its length, a CRC-32
   of its contents and its path */
static int cmd_arc(const char *file)
{
	char error[200];
	adfs_file_t *f;
	size_t len;
	uint8_t *d = file_read(file, &len);
	int n = d ? arc_files(d, len, &f, error, sizeof error) : -1, i;

	if (n < 0)
	{
		fprintf(stderr, "%s\n", d ? error : "can't read it");
		free(d);
		return 1;
	}
	free(d);
	for (i = 0; i < n; i++)
		if (f[i].dir)
			printf("dir                                %s\n", f[i].path);
		else
			printf("%08x %08x %7u %08lx %s\n", (unsigned)f[i].load, (unsigned)f[i].exec, (unsigned)f[i].len,
			       crc32(0, f[i].data, f[i].len), f[i].path);
	adfs_free(f, n);
	return 0;
}

/* The checks that need nothing but the program itself */
static int cmd_check(void)
{
	uint8_t *rgba = malloc(128 * 128 * 4), *back;
	char path[1024];
	int w, h, fails = 0;

	if (disc_selftest())
	{
		printf("FAIL: the disc images, check %d\n", disc_selftest());
		fails++;
	}
	if (adfs_selftest())
	{
		printf("FAIL: an ADFS disc made from files\n");
		fails++;
	}
	if (arc_selftest())
	{
		printf("FAIL: a Spark or ArcFS archive\n");
		fails++;
	}
	icon_app(rgba);
	if (rgba[(64 * 128 + 64) * 4 + 3] != 255 || rgba[3] != 0)
	{
		printf("FAIL: the icon's circle\n");
		fails++;
	}
	snprintf(path, sizeof path, "%s/check.png", store_dir());
	back = png_write(path, rgba, 128, 128) ? NULL : png_read(path, &w, &h);
	if (!back || w != 128 || h != 128 || memcmp(back, rgba, 128 * 128 * 4))
	{
		printf("FAIL: a PNG didn't read back as it was written\n");
		fails++;
	}
	remove(path);
	if (catalog_count() < 100)
	{
		printf("FAIL: the game table\n");
		fails++;
	}
	{
		/* names and the disc each says it is, and how long the title before it is */
		static const struct { const char *name; int disc, at; } clues[] = {
			{ "Lemmings (Disk 2 of 3).adf", 2, 8 }, { "Zarch_disc_1.adf", 1, 5 }, { "Game 3 of 4.ADF", 3, 4 },
			{ "Pacmania - Side B.adl", 2, 8 }, { "Gods (1991) (Bitmap) F1022702.jfd", 2, 21 },
			{ "Lemmings 2.adf", 2, -1 }, { "Chuckie Egg.hfe", 0, -1 }, { "Disk1of3.jfd", 1, 0 },
		};
		size_t i;
		int at;

		for (i = 0; i < sizeof clues / sizeof *clues; i++)
			if (disc_clue(clues[i].name, &at) != clues[i].disc || at != clues[i].at)
			{
				printf("FAIL: the disc %s's name gives\n", clues[i].name);
				fails++;
			}
	}
	printf(fails ? "%d checks failed\n" : "all checks pass\n", fails);
	return fails != 0;
}

int main(int argc, char **argv)
{
	const char *cmd = argc > 1 ? argv[1] : "";
	const char *script = NULL;
	int i;

	for (i = 2; i + 1 < argc; i++)
		if (!strcmp(argv[i], "--test"))
			script = argv[i + 1];
	if (!strcmp(cmd, "play") && argc > 2)
		return game_main(argv[2], script);
	if (!strcmp(cmd, "test"))
		return cmd_test(argc - 2, argv + 2);
	if (!strcmp(cmd, "mkicons"))
		return cmd_mkicons(argc - 2, argv + 2);
	if (!strcmp(cmd, "icon"))
		return cmd_icon(argc - 2, argv + 2);
	if (!strcmp(cmd, "setup"))
		return setup_main(argc - 2, argv + 2, script);
	if (!strcmp(cmd, "check"))
		return cmd_check();
	if (!strcmp(cmd, "arc") && argc > 2)            /* the files in a Spark or ArcFS archive, for the test scripts */
		return cmd_arc(argv[2]);
	if (!strcmp(cmd, "ls") && argc > 2)             /* the files on a disc image, for writing presets */
	{
		char name[11];
		size_t len;
		uint8_t *img = disc_sectors(argv[2], &len);
		adfs_file_t *files;
		int boot, n = img ? adfs_read(img, len, &files, name, &boot) : -1;

		free(img);
		if (n < 0)
		{
			fprintf(stderr, "can't read the disc in %s\n", argv[2]);
			return 1;
		}
		printf("disc name %s\nboot option %d\n", name, boot);
		for (i = 0; i < n; i++)
			if (files[i].dir)
				printf("%s\n", files[i].path);
			else
				printf("%-40s %8u  %08x %08x\n", files[i].path, (unsigned)files[i].len, (unsigned)files[i].load, (unsigned)files[i].exec);
		adfs_free(files, n);
		return 0;
	}
	if (!strcmp(cmd, "extract") && argc > 3)        /* what is in an archive or on a disc image, to look into a game */
	{
		char error[200] = "it's no archive or disc image PiArcBoot reads";
		size_t len, ilen;
		uint8_t *d = file_read(argv[2], &len), *img;
		adfs_file_t *files;
		int n = -1;

		if (d && len > 4 && !memcmp(d, "PK\3\4", 4))
			n = adfs_from_zip(d, len, "", &files);
		else if (d && arc_is_archive(d, len))
			n = arc_files(d, len, &files, error, sizeof error);
		else if ((img = disc_sectors(argv[2], &ilen)))
		{
			n = adfs_read(img, ilen, &files, NULL, NULL);
			free(img);
		}
		free(d);
		if (n < 0 || make_dirs(argv[3]) || write_hostfs(argv[3], files, n))
		{
			fprintf(stderr, "%s: %s\n", argv[2], n < 0 ? error : "can't write the files");
			return 1;
		}
		printf("%d\n", n);
		adfs_free(files, n);
		return 0;
	}
	if (!strcmp(cmd, "add") && argc > 2)            /* a game from disc images or archives, as Add Game makes one */
	{
		char id[80], error[240];
		progress_t p;
		game_t g;

		memset(&p, 0, sizeof p);
		if (import_game(argv + 2, argc - 2, NULL, id, error, sizeof error))
		{
			fprintf(stderr, "%s\n", error);
			return 1;
		}
		if (!store_load(id, &g))
		{
			if (run_installer(&g, id, &p) < 0)
				fprintf(stderr, "%s\n", p.error);
			store_save(&g);
		}
		printf("%s\n", id);
		return 0;
	}
	if (!strcmp(cmd, "install") && argc > 2)        /* a game from the JASPP list, as Install does */
	{
		progress_t p;

		memset(&p, 0, sizeof p);
		for (i = 0; i < catalog_count(); i++)
			if (!strcmp(catalog_entry(i)->id, argv[2]))
			{
				if (install_jaspp(catalog_entry(i), 0, 0, &p))
					fprintf(stderr, "%s\n", p.error);
				return p.done != 1;
			}
		fprintf(stderr, "no game called %s in the JASPP list\n", argv[2]);
		return 1;
	}
	if (!strcmp(cmd, "classic") && argc == 2)       /* fetch the PackMan sources and list the Classics tab's games */
	{
		classics_refresh();
		classics_load();
		for (i = 0; i < classic_count(); i++)
			printf("%s\t%s\t%s\t%s\t%s\t%s\t%d\n", classic_entry(i)->id, classic_entry(i)->boot, classic_entry(i)->prefix, classic_entry(i)->url,
			       classic_entry(i)->title, classic_entry(i)->info, classic_entry(i)->section);
		return 0;
	}
	if (!strcmp(cmd, "classic"))                    /* install a game from the Classics tab, as Install does */
	{
		progress_t p;

		memset(&p, 0, sizeof p);
		classics_load();
		for (i = 0; i < classic_count(); i++)
			if (!strcmp(classic_entry(i)->id, argv[2]))
			{
				if (install_classic(classic_entry(i), 0, 0, &p))
					fprintf(stderr, "%s\n", p.error);
				return p.done != 1;
			}
		fprintf(stderr, "no game called %s in the Classics list\n", argv[2]);
		return 1;
	}
	if (!strcmp(cmd, "checknew"))           /* look for floppy images of the games the feed has and the table doesn't */
		return !feed_check_new();
	if (!strcmp(cmd, "list"))               /* the game table, a line a game, for the test scripts */
	{
		for (i = 0; i < catalog_count(); i++)
		{
			const entry_t *e = catalog_entry(i);

			printf("%s\t%s\t%d\t%d\t%05u\t%s\n", e->id, e->boot[0] ? e->boot : "-", e->fdc, e->wp, e->game, e->version);
		}
		return 0;
	}
	if (!cmd[0] || !strcmp(cmd, "launcher"))
		return launcher_main(script);
	fprintf(stderr, "usage: piarcboot [play <game>]\n");
	return 2;
}
