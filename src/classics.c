/* The Classics tab's games: freeware that comes as RISC OS applications rather than as disc
   images. Acorn Arcade's former commercial games, which their authors and publishers let it give
   away, and the games in PackMan's package sources that need nothing past RISC OS 3.11.

   A game's zip comes from its copy in PiArcBoot's repository, under distributable-games/, and
   from where its publisher put it if that copy isn't there. Installing it makes ADFS floppy
   images of what the zip holds (adfs.c): its applications, each whole on one disc, the one the
   game starts from on the first. */
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>
#include "piarcboot.h"
#include "hostfsmod.h"

#define MIRROR "https://raw.githubusercontent.com/jameshansen/PiArcBoot/main/distributable-games/"
#define ARCADE "http://www.acornarcade.com/downloads/"
#define ASYLUM "http://asylum.acornarcade.com/files/"
#define SUPPORT "https://github.com/jameshansen/ROO311/releases/latest/download/"   /* where ROO311 publishes its support packs */
#define DISC_ROOM (800 * 1024 - 0xC00)  /* an 800K disc less its map and root directory */
#define BIG_ROOM (DISC_ROOM + 4 * 10240) /* and with the 4 tracks more an object too big for that may have */
#define MAX_PACKMAN 200

/* Acorn Arcade's free games. Of the others there, DarkWood needs a Risc PC, and Phaethon its two
   discs in two drives. Terramex keeps time by VSync: its own patch halves the VSyncs on the
   faster monitors of later machines, and at this machine's 50 Hz it plays at its own speed. */
static const classic_t arcade[] = {
	{ "Asylum", "Asylum", "1994, Andy Southgate", "asylum.acornarcade/asy202.zip", ASYLUM "asy202.zip", "AsyNu/", "!Asylum", CLASSIC_ARCADE },
	{ "Big-Bang", "Big Bang", "Psycore, from PomPom Games", "acornarcade/bigbang.zip", ARCADE "bigbang.zip", "", "!BigBang", CLASSIC_ARCADE },
	{ "Cycloids", "Cycloids", "1993, Tom Cooper, Software 42", "acornarcade/cycloids.zip", ARCADE "cycloids.zip", "Cycloids/", "!Cycloids", CLASSIC_ARCADE },
	{ "DinoSaw", "DinoSaw", "Tom Cooper", "acornarcade/dinosaw.zip", ARCADE "dinosaw.zip", "DinoSaw/", "!DinoSaw", CLASSIC_ARCADE },
	/* its application is !Élite, with the é of RISC OS's Latin-1, which HostLink starts as it is */
	{ "ArcElite", "Elite", "1991, Warren Burch and Clive Gringras after Ian Bell and David Braben, Hybrid Technology",
	  "acornarcade/elite.zip", ARCADE "elite.zip", "elite/", "!\xE9lite", CLASSIC_ARCADE },
	{ "Hamsters", "Hamsters", "1994, Tom Cooper, Gamesware", "acornarcade/hamsters.zip", ARCADE "hamsters.zip", "", "!Hamsters", CLASSIC_ARCADE },
	{ "Ixion", "Ixion", "1992, Tom Cooper and Barry Clarkson, Software 42", "acornarcade/ixion.zip", ARCADE "ixion.zip", "Ixion/", "!Ixion", CLASSIC_ARCADE },
	{ "Oddball", "Oddball", "1995, Andy Southgate", "asylum.acornarcade/oddball.zip", ASYLUM "oddball.zip", "OddballFD/", "!OddBall", CLASSIC_ARCADE },
	{ "Revolver", "Revolver", "Psycore, from PomPom Games", "acornarcade/revolver.zip", ARCADE "revolver.zip", "", "!revolver", CLASSIC_ARCADE },
	/* release 3.21, free from its own site, which is where it comes from (its presets give it ROO311's support pack) */
	{ "Star-Fighter-3000", "Star Fighter 3000", "1994, Fednet Software, release 3.21 by Christopher Bazley", "",
	  "http://starfighter.acornarcade.com/download/sf3000.zip", "", "!Star3000", CLASSIC_ARCADE },
	{ "Terramex", "Terramex", "1987, Shaun Hollingworth and Pete Harrap, Krisalis Software", "acornarcade/terra.zip",
	  ARCADE "terra.zip", "", "!Terramex", CLASSIC_ARCADE },
};

/* The PackMan sources the tab starts with: RISC OS Open's own and the third party one it hosts */
static const char *const default_sources[] = {
	"http://packages.riscosopen.org/packages/pkg/rool",
	"http://packages.riscosopen.org/packages/pkg/thirdparty",
};

static classic_t packman[MAX_PACKMAN];
static int n_packman;

int classic_count(void)
{
	return (int)(sizeof arcade / sizeof *arcade) + n_packman;
}

const classic_t *classic_entry(int i)
{
	int n = sizeof arcade / sizeof *arcade;

	return i < n ? &arcade[i] : &packman[i - n];
}

/* ---- PackMan sources ---- */

static void sources_file(char *buf, size_t size)
{
	snprintf(buf, size, "%s/sources.txt", store_dir());
}

int classics_sources(char urls[][300], int max)
{
	char path[1024], *text, *line;
	int n = 0;
	size_t i;

	sources_file(path, sizeof path);
	text = (char *)file_read(path, NULL);
	if (!text)
	{
		for (i = 0; i < sizeof default_sources / sizeof *default_sources && n < max; i++)
			snprintf(urls[n++], 300, "%s", default_sources[i]);
		return n;
	}
	for (line = strtok(text, "\r\n"); line && n < max; line = strtok(NULL, "\r\n"))
		if (!strncmp(line, "http://", 7) || !strncmp(line, "https://", 8))
			snprintf(urls[n++], 300, "%s", line);
	free(text);
	return n;
}

static void save_sources(char urls[][300], int n)
{
	char path[1024];
	FILE *f;
	int i;

	sources_file(path, sizeof path);
	if (!(f = fopen(path, "w")))
		return;
	for (i = 0; i < n; i++)
		fprintf(f, "%s\n", urls[i]);
	fclose(f);
}

void classics_add_source(const char *url)
{
	static char urls[32][300];
	int n = classics_sources(urls, 32), i;

	if ((strncmp(url, "http://", 7) && strncmp(url, "https://", 8)) || strpbrk(url, " \"'`$\\") || n >= 32)
		return;
	for (i = 0; i < n; i++)
		if (!strcmp(urls[i], url))
			return;
	snprintf(urls[n++], 300, "%s", url);
	save_sources(urls, n);
}

void classics_remove_source(int k)
{
	static char urls[32][300];
	int n = classics_sources(urls, 32);

	if (k < 0 || k >= n)
		return;
	memmove(urls[k], urls[k + 1], (size_t)(n - k - 1) * 300);
	save_sources(urls, n - 1);
}

/* Where a source's index is kept in the data folder */
static void index_file(const char *url, char *buf, size_t size)
{
	snprintf(buf, size, "%s/source-%08lx.txt", store_dir(), crc32(0, (const Bytef *)url, (uInt)strlen(url)));
}

/* A package's URL as its index gives it, which may be relative to the index's own */
static void resolve(const char *base, const char *rel, char *out, size_t size)
{
	char dir[400];
	char *slash;

	if (!strncmp(rel, "http://", 7) || !strncmp(rel, "https://", 8))
	{
		snprintf(out, size, "%s", rel);
		return;
	}
	snprintf(dir, sizeof dir, "%s", base);
	if ((slash = strrchr(dir, '/')))
		*slash = 0;
	while (!strncmp(rel, "../", 3) && (slash = strrchr(dir, '/')) && slash > strstr(dir, "//") + 1)
	{
		*slash = 0;
		rel += 3;
	}
	while (!strncmp(rel, "./", 2))
		rel += 2;
	snprintf(out, size, "%s/%s", dir, rel);
}

/* The games in one index that may run on RISC OS 3.11: in the Games section, free, needing no
   other package, and for any processor rather than ARM code built for later machines. A package
   found not to run anyway has a skip setting in its presets, saying why. */
static void parse_index(const char *url, char *text)
{
	char *rec = text;

	while (rec && *rec && n_packman < MAX_PACKMAN)
	{
		char *end = strstr(rec, "\n\n"), *line, *next;
		char name[80] = "", section[40] = "", licence[40] = "", env[40] = "", desc[160] = "", where[300] = "", comp[200] = "";
		int depends = 0;

		if (end)
			*end = 0;
		for (line = rec; line && *line; line = next)
		{
			char *colon;

			next = strchr(line, '\n');
			if (next)
				*next++ = 0;
			line[strcspn(line, "\r")] = 0;
			if (line[0] == ' ' || !(colon = strchr(line, ':')))
				continue;
			*colon = 0;
			for (colon++; *colon == ' '; colon++);
			if (!strcmp(line, "Package")) snprintf(name, sizeof name, "%s", colon);
			else if (!strcmp(line, "Section")) snprintf(section, sizeof section, "%s", colon);
			else if (!strcmp(line, "Licence")) snprintf(licence, sizeof licence, "%s", colon);
			else if (!strcmp(line, "Environment")) snprintf(env, sizeof env, "%s", colon);
			else if (!strcmp(line, "Description")) snprintf(desc, sizeof desc, "%s", colon);
			else if (!strcmp(line, "URL")) snprintf(where, sizeof where, "%s", colon);
			else if (!strcmp(line, "Components")) snprintf(comp, sizeof comp, "%s", colon);
			else if (!strcmp(line, "Depends")) depends = 1;
		}
		rec = end ? end + 2 : NULL;
		if (!name[0] || strcmp(section, "Games") || strcmp(licence, "Free") || depends || (env[0] && strcmp(env, "any")) || !where[0])
			continue;
		{
			classic_t *c = &packman[n_packman];
			char *bang, *dot, id[90], why[200];
			int i, dup = 0;

			snprintf(id, sizeof id, "pkg-%s", name);
			if (preset_setting(id, "skip", why, sizeof why))
				continue;
			for (i = 0; i < n_packman; i++)
				dup |= !strcmp(packman[i].title, name);
			if (dup)
				continue;
			memset(c, 0, sizeof *c);
			snprintf(c->id, sizeof c->id, "%s", id);
			snprintf(c->title, sizeof c->title, "%s", name);
			snprintf(c->info, sizeof c->info, "%s", desc);
			resolve(url, where, c->url, sizeof c->url);
			/* Components: Apps.Games.!Hopper (Movable) puts the application in Apps/Games/ in the zip */
			if ((bang = strchr(comp, '!')))
			{
				bang[strcspn(bang, " ,")] = 0;
				snprintf(c->boot, sizeof c->boot, "%s", bang);
				*bang = 0;
				for (dot = comp; *dot; dot++)
					if (*dot == '.')
						*dot = '/';
				snprintf(c->prefix, sizeof c->prefix, "%s", comp);
			}
			c->section = CLASSIC_PACKMAN;
			n_packman++;
		}
	}
}

void classics_load(void)
{
	static char urls[32][300];
	int n = classics_sources(urls, 32), i;

	n_packman = 0;
	for (i = 0; i < n; i++)
	{
		char path[1024];
		char *text;

		index_file(urls[i], path, sizeof path);
		if ((text = (char *)file_read(path, NULL)))
		{
			parse_index(urls[i], text);
			free(text);
		}
	}
}

int classics_refresh(void)
{
	static char urls[32][300];
	int n = classics_sources(urls, 32), i, got = 0;

	for (i = 0; i < n; i++)
	{
		char path[1024], tmp[1100];
		uint8_t *text;

		index_file(urls[i], path, sizeof path);
		snprintf(tmp, sizeof tmp, "%s.new", path);
		if (store_fetch(urls[i], tmp))
		{
			unlink(tmp);
			continue;
		}
		text = file_read(tmp, NULL);
		if (text && strstr((char *)text, "Package:"))
		{
			unlink(path);
			got |= !rename(tmp, path);
		}
		free(text);
		unlink(tmp);
	}
	return got;
}

/* ---- Installing ---- */

static int fail(progress_t *p, const char *why)
{
	snprintf(p->error, sizeof p->error, "%s", why);
	p->done = -1;
	return -1;
}

/* The first part of a RISC OS path, the object in the disc's root it belongs to */
static size_t top_len(const char *path)
{
	return strcspn(path, ".");
}

/* The room the object in the root that f[i] belongs to takes on a disc: its files in whole 1024
   byte sectors, and 2048 bytes for each folder, counted once whether or not the zip lists it */
static uint32_t object_room(const adfs_file_t *f, int n, int i)
{
	size_t tl = top_len(f[i].path), (*dirs)[2] = malloc(sizeof *dirs * n * 8);
	uint32_t size = 0;
	int nd = 0, j, d;

	for (j = 0; j < n; j++)
	{
		const char *path = f[j].path, *dot;

		if (top_len(path) != tl || strncmp(path, f[i].path, tl))
			continue;
		if (!f[j].dir)
			size += f[j].len ? (f[j].len + 1023) / 1024 * 1024 : 1024;
		for (dot = path; ; dot++)       /* each folder on its way, and itself if it is one */
		{
			if (*dot == '.' || (!*dot && f[j].dir))
			{
				size_t k = (size_t)(dot - path);

				for (d = 0; d < nd && !(dirs[d][1] == k && !strncmp(f[dirs[d][0]].path, path, k)); d++);
				if (d == nd && nd < n * 8)
				{
					dirs[nd][0] = (size_t)j;
					dirs[nd++][1] = k;
				}
			}
			if (!*dot)
				break;
		}
	}
	free(dirs);
	return size + (uint32_t)nd * 0x800;
}

/* Which disc each file goes on: the application the game starts from first, then each other
   object in the zip's root in turn, on the first disc with room for it whole. Directories the
   zip doesn't list are counted too. Returns the number of discs, or 0 if an object is bigger
   than a disc. */
static int pack(adfs_file_t *f, int n, const char *boot, int *disc)
{
	uint32_t used[MAX_DISCS] = { 0 };
	int ndiscs = 0, i, k, pass;

	for (i = 0; i < n; i++)
		disc[i] = -1;
	for (pass = 0; pass < 2; pass++)
		for (i = 0; i < n; i++)
		{
			size_t tl = top_len(f[i].path);
			uint32_t size;
			int is_boot = strlen(boot) == tl && !strncmp(f[i].path, boot, tl), j;

			if (disc[i] >= 0 || is_boot != !pass)
				continue;
			size = object_room(f, n, i);
			if (size > BIG_ROOM)
				return 0;
			for (k = 0; k < ndiscs && used[k] + size > (used[k] ? DISC_ROOM : BIG_ROOM); k++);
			if (k == ndiscs)
			{
				if (ndiscs == MAX_DISCS)
					return 0;
				ndiscs++;
			}
			used[k] += size;
			for (j = 0; j < n; j++)
				if (top_len(f[j].path) == tl && !strncmp(f[j].path, f[i].path, tl))
					disc[j] = k;
		}
	return ndiscs;
}

/* A RISC OS path as HostFS names it on the computer: the folders' . and the name's own / change
   places, a hard space is a space, ? < > are # $ ^, and a file's type follows a comma, or its load
   and execution addresses for one with no type. 1 for a name that would leave the folder (a part
   that comes out as . or ..). */
static int host_name(const adfs_file_t *f, char *out, size_t size)
{
	static const char from[] = "./\\\xA0?<>", to[] = "/._ #$^";
	size_t i, k = 0, part = 0;

	for (i = 0; f->path[i] && k + 1 < size; i++)
	{
		const char *swap = strchr(from, f->path[i]);
		char ch = swap ? to[swap - from] : f->path[i];

		out[k++] = ch;
		if (ch == '/' || !f->path[i + 1])
		{
			size_t end = ch == '/' ? k - 1 : k;

			if ((end - part == 1 && out[part] == '.') || (end - part == 2 && out[part] == '.' && out[part + 1] == '.'))
				return 1;
			part = k;
		}
	}
	out[k] = 0;
	if (f->dir || !k)
		return !k;
	if ((f->load & 0xFFF00000u) != 0xFFF00000u)
		snprintf(out + k, size - k, ",%x-%x", (unsigned)f->load, (unsigned)f->exec);
	else if (((f->load >> 8) & 0xFFF) != 0xFFF)
		snprintf(out + k, size - k, ",%03x", (unsigned)(f->load >> 8) & 0xFFF);
	return 0;
}

/* Writes a file, making the folders it goes in */
static int put_file(const char *path, const void *data, size_t len)
{
	char dir[1400];
	char *slash;

	snprintf(dir, sizeof dir, "%s", path);
	slash = strrchr(dir, '/');
	if (slash)
	{
		*slash = 0;
		if (make_dirs(dir))
			return 1;
	}
	return file_write(path, data, len);
}

/* A game that needs a support pack, or has something in it too big for a floppy, goes in a folder
   HostFS shows the machine, hostfs/, with the pack's !Boot, !System and !Scrap beside it. The
   pack's extrom/ folder, and HostFS itself, make the extension ROM the machine starts with,
   extrom/. support is "" for no pack. */
int write_hostfs(const char *dir, const adfs_file_t *f, int n)
{
	char path[1400], name[1024];
	int i;

	for (i = 0; i < n; i++)
	{
		if (host_name(&f[i], name, sizeof name))
			continue;
		snprintf(path, sizeof path, "%s/%s", dir, name);
		if (f[i].dir ? make_dirs(path) : put_file(path, f[i].data, f[i].len))
			return -1;
	}
	return 0;
}

static int install_hostfs(const char *title, const char *support, const adfs_file_t *f, int n, const char *part, progress_t *p)
{
	char path[1400], url[600];
	zipent_t *ents;
	uint8_t *zip;
	size_t len;
	int i, m;

	snprintf(p->step, sizeof p->step, "Putting %s in its folder...", title);
	snprintf(path, sizeof path, "%s/hostfs", part);
	if (write_hostfs(path, f, n))
		return fail(p, "The game's files can't be saved.");
	snprintf(path, sizeof path, "%s/extrom/HostFS,ffa", part);
	if (put_file(path, hostfs_module, sizeof hostfs_module))
		return fail(p, "The game's files can't be saved.");
	if (!support[0])
		return 0;

	snprintf(p->step, sizeof p->step, "Downloading the support pack...");
	snprintf(path, sizeof path, "%s/support.zip", part);
	snprintf(url, sizeof url, MIRROR "support-packs/%s.zip", support);
	if (store_fetch(url, path))
	{
		snprintf(url, sizeof url, SUPPORT "%s.zip", support);
		if (store_fetch(url, path))
			return fail(p, "The support pack didn't arrive. Check the network connection and try again.");
	}
	zip = file_read(path, &len);
	unlink(path);
	/* its names are HostFS's already, types after a comma */
	m = zip ? zip_list(zip, len, &ents) : 0;
	for (i = 0; i < m; i++)
	{
		const char *e = ents[i].name;
		size_t elen = strlen(e);
		uint8_t *data;

		if (e[0] == '/' || strstr(e, "..") || strchr(e, '\\'))
			continue;
		if (!strncmp(e, "extrom/", 7))
			snprintf(path, sizeof path, "%s/extrom/%s", part, e + 7);
		else
			snprintf(path, sizeof path, "%s/hostfs/%s", part, e);
		if (elen && e[elen - 1] == '/')
		{
			make_dirs(path);
			continue;
		}
		data = zip_get(zip, len, &ents[i], &elen);
		if (!data || put_file(path, data, elen))
		{
			free(data);
			free(ents);
			free(zip);
			return fail(p, "The support pack can't be saved.");
		}
		free(data);
	}
	if (m > 0)
		free(ents);
	free(zip);
	if (m <= 0)
		return fail(p, "The support pack isn't a zip.");
	return 0;
}

/* The support pack applications ask for in their !Run files, as RISC OS 3.11 has none of these:
   the Toolbox, the nested Window Manager, System:, a scrap folder or Choices:. "" for none. */
const char *needs_support(const adfs_file_t *f, int n)
{
	static const char *const asks[] = { "toolbox", "system$path", "wimp$scrapdir", "choices:", "choices$",
					    "windowmanager 3.8", "windowmanager 3.9", "windowmanager 4" };
	int i;
	size_t k, j;

	for (i = 0; i < n; i++)
	{
		const char *dot = strchr(f[i].path, '.');
		char *low;

		if (f[i].dir || f[i].path[0] != '!' || !dot || strcmp(dot, ".!Run") || !f[i].data)
			continue;
		low = malloc(f[i].len + 1);
		for (k = 0; k < f[i].len; k++)
			low[k] = (char)tolower(f[i].data[k] ? f[i].data[k] : ' ');
		low[k] = 0;
		for (j = 0; j < sizeof asks / sizeof *asks && !strstr(low, asks[j]); j++);
		free(low);
		if (j < sizeof asks / sizeof *asks)
			return "toolbox";
	}
	return "";
}

/* Lays a game's RISC OS files out in its folder dir: on ADFS floppy images, disc1.adf onwards,
   with the application it starts from on the first, or in a HostFS folder when support names a
   pack or something in it is too big for a floppy. Fills in g's discs. */
int layout_files(game_t *g, const char *dir, adfs_file_t *files, int n, const char *boot, const char *support, progress_t *p)
{
	char path[1100], error[240];
	adfs_file_t *subset;
	int *disc, ndiscs, i, k;

	if (strspn(support, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-") != strlen(support))
		support = "";
	if (support[0])
		return install_hostfs(g->title, support, files, n, dir, p);
	disc = malloc((size_t)n * sizeof *disc);
	subset = malloc((size_t)n * sizeof *subset);
	ndiscs = pack(files, n, boot, disc);
	if (ndiscs)
		snprintf(p->step, sizeof p->step, "Making %s's discs...", g->title);
	for (k = 0; k < ndiscs; k++)
	{
		uint8_t *img;
		size_t size;
		int m = 0;

		for (i = 0; i < n; i++)
			if (disc[i] == k)
				subset[m++] = files[i];
		img = adfs_make(subset, m, g->title, &size, error, sizeof error);
		snprintf(g->disc[k], sizeof g->disc[0], "disc%d.adf", k + 1);
		snprintf(path, sizeof path, "%s/%s", dir, g->disc[k]);
		if (!img || file_write(path, img, size))
		{
			free(img);
			free(disc);
			free(subset);
			return fail(p, img ? "The game's discs can't be saved." : error);
		}
		free(img);
	}
	g->ndiscs = ndiscs;
	free(disc);
	free(subset);
	return ndiscs ? 0 : install_hostfs(g->title, "", files, n, dir, p);   /* too big for floppies */
}

/* ---- Presets ----

   Files PiArcBoot adds to a game as it installs it, such as a choices file that sets the game up
   for its window, from presets/<game>/ beside the program (presets/README.md in the repository).
   They are laid out as the game's own folder is and named as HostFS names files: the RISC OS / as
   the computer's ., and a file's type after a comma, or its load and execution addresses. A file
   with no type is text. A preset takes the place of the game's own file of the same name. */
static void add_preset(const char *host, const char *rel, adfs_file_t **files, int *n)
{
	char path[512];
	const char *comma = strrchr(rel, ','), *slash = strrchr(rel, '/');
	uint32_t load = 0xFFFFFF41u, exec = 0x42F2D200u;        /* text, stamped 1 January 1992 like a zip's files */
	size_t len, end = strlen(rel), i, k = 0;
	unsigned a, b;
	uint8_t *data;
	int j;

	if (comma && (!slash || comma > slash))
	{
		if (strlen(comma + 1) == 3 && strspn(comma + 1, "0123456789abcdefABCDEF") == 3)
		{
			load = 0xFFF00041u | (uint32_t)strtoul(comma + 1, NULL, 16) << 8;
			end = (size_t)(comma - rel);
		}
		else if (sscanf(comma + 1, "%8x-%8x", &a, &b) == 2)
		{
			load = a;
			exec = b;
			end = (size_t)(comma - rel);
		}
	}
	for (i = 0; i < end && k + 1 < sizeof path; i++)
		path[k++] = rel[i] == '/' ? '.' : rel[i] == '.' ? '/' : rel[i];
	path[k] = 0;
	data = file_read(host, &len);
	if (!data)
		return;
	for (j = 0; j < *n; j++)        /* the game's own file of that name, if it has one; RISC OS ignores case */
	{
		const char *x = (*files)[j].path, *y = path;

		for (; *x && tolower((unsigned char)*x) == tolower((unsigned char)*y); x++, y++);
		if (!*x && !*y)
			break;
	}
	if (j == *n)
	{
		*files = realloc(*files, (size_t)(*n + 1) * sizeof **files);
		memset(&(*files)[j], 0, sizeof **files);
		(*files)[j].path = strdup(path);
		(*files)[j].attr = 3;
		(*n)++;
	}
	else
		free((uint8_t *)(*files)[j].data);
	(*files)[j].load = load;
	(*files)[j].exec = exec;
	(*files)[j].data = data;
	(*files)[j].len = (uint32_t)len;
	(*files)[j].dir = 0;
}

static void add_presets_in(const char *dir, const char *rel, adfs_file_t **files, int *n)
{
	DIR *d = opendir(dir);
	struct dirent *e;

	while (d && (e = readdir(d)))
	{
		char host[1400], sub[700];
		struct stat st;

		if (e->d_name[0] == '.')
			continue;
		snprintf(host, sizeof host, "%s/%s", dir, e->d_name);
		snprintf(sub, sizeof sub, "%s%s%s", rel, rel[0] ? "/" : "", e->d_name);
		if (stat(host, &st))
			continue;
		if (S_ISDIR(st.st_mode))
			add_presets_in(host, sub, files, n);
		else
			add_preset(host, sub, files, n);
	}
	if (d)
		closedir(d);
}

static void add_presets(const char *id, adfs_file_t **files, int *n)
{
	char name[160], dir[1024];

	snprintf(name, sizeof name, "presets/%s", id);
	if (store_asset(name, dir, sizeof dir))
		add_presets_in(dir, "", files, n);
}

/* presets/<game>.ini: a key=value line for each setting, with # starting a comment line */
int preset_setting(const char *id, const char *key, char *buf, size_t size)
{
	char name[160], path[1024];
	char *text, *line, *next;
	size_t klen = strlen(key);
	int found = 0;

	snprintf(name, sizeof name, "presets/%s.ini", id);
	if (!store_asset(name, path, sizeof path) || !(text = (char *)file_read(path, NULL)))
		return 0;
	for (line = text; line && *line && !found; line = next)
	{
		next = strchr(line, '\n');
		if (next)
			*next++ = 0;
		line[strcspn(line, "\r")] = 0;
		while (*line == ' ' || *line == '\t')
			line++;
		if (!strncmp(line, key, klen) && line[klen] == '=')
		{
			snprintf(buf, size, "%s", line + klen + 1);
			found = 1;
		}
	}
	free(text);
	return found;
}

/* ---- Installers ----

   A game that came with a hard disc installer has presets/<game>.install, the files its installer
   copies from its discs and where they go (presets/README.md). run_installer copies them from the
   disc images onto the game's HostFS folder, and keeps them in installed/, which deploy_installed
   puts back as the game's HostFS folder whenever that is missing. */

static int same_name(const char *a, const char *b)
{
	for (; *a && tolower((unsigned char)*a) == tolower((unsigned char)*b); a++, b++);
	return !*a && !*b;
}

/* The files on one of a game's discs, from 0, or -1 if it can't be read */
static int disc_files(const game_t *g, int k, adfs_file_t **files)
{
	char path[1100];
	size_t len;
	uint8_t *img;
	int n;

	store_game_path(g->id, g->disc[k], path, sizeof path);
	img = disc_sectors(path, &len);
	n = img ? adfs_read(img, len, files, NULL, NULL) : -1;
	free(img);
	return n;
}

/* What of path is inside the folder or file pattern names, from the first part of it in, or NULL
   if it isn't there. Parts compare without regard to case, with # for any one character, and an
   empty pattern is the disc's root. */
static const char *inside(const char *path, const char *pattern)
{
	if (!*pattern)
		return path;
	for (; *pattern; path++, pattern++)
		if (*pattern == '.' ? *path != '.' : !*path || *path == '.' ||
		    (*pattern != '#' && tolower((unsigned char)*pattern) != tolower((unsigned char)*path)))
			return NULL;
	return !*path ? path : *path == '.' ? path + 1 : NULL;
}

/* The copy list for the game with that id, or for a game added by hand the one whose
   presets/<game>.ini has a match= line naming files that are all at the root of its first disc,
   with # for any one character */
static char *installer_list(const game_t *g, const char *id, char *buf, size_t size)
{
	char name[200], dir[1024], match[400];
	adfs_file_t *files;
	struct dirent *d;
	DIR *dd;
	int n, i;

	snprintf(name, sizeof name, "presets/%s.install", id);
	if (store_asset(name, buf, size))
		return buf;
	if (!g->ndiscs || !store_asset("presets", dir, sizeof dir))
		return NULL;
	n = disc_files(g, 0, &files);
	dd = n > 0 ? opendir(dir) : NULL;
	while (dd && (d = readdir(dd)))
	{
		size_t len = strlen(d->d_name);
		char other[100], *word;
		const char *rest;
		int all = 1;

		if (len < 5 || len >= sizeof other || strcmp(d->d_name + len - 4, ".ini"))
			continue;
		snprintf(other, sizeof other, "%.*s", (int)(len - 4), d->d_name);
		if (!preset_setting(other, "match", match, sizeof match))
			continue;
		for (word = match + strspn(match, " \t"); *word && all; word += strspn(word, " \t"))
		{
			size_t wlen = strcspn(word, " \t");
			char one = word[wlen];

			word[wlen] = 0;
			for (i = 0; i < n && (!(rest = inside(files[i].path, word)) || *rest); i++);
			all = i < n;
			word[wlen] = one;
			word += wlen;
		}
		snprintf(name, sizeof name, "presets/%s.install", other);
		if (all && store_asset(name, buf, size))
		{
			closedir(dd);
			adfs_free(files, n);
			return buf;
		}
	}
	if (dd)
		closedir(dd);
	adfs_free(files, n);
	return NULL;
}

int has_installer(const game_t *g, const char *id)
{
	char list[1024];

	return installer_list(g, id, list, sizeof list) != NULL;
}

/* Copies a game's files from its discs onto its HostFS folder, as the copy list its presets give
   it says, with id the game's own (a JASPP game's folder is <id>.part while it installs). Each
   line is "copy <disc> <from> <to>", a folder with all in it or a file, and a later copy takes the
   place of an earlier one of the same name. 0 when it installed or there is no list, 1 when it
   can't yet for want of a disc the game doesn't have, -1 with p->error saying why when it failed.
   A game whose install fails still plays from its floppies. */
int run_installer(game_t *g, const char *id, progress_t *p)
{
	char list[1024], dir[1024], path[1300], hostfs[1100], *text, *line, *next;
	adfs_file_t *disc[MAX_DISCS], *out = NULL;
	int ndisc[MAX_DISCS], nout = 0, result = 0, installed = 0, i;
	DIR *d;
	struct dirent *e;

	if (!installer_list(g, id, list, sizeof list) || !(text = (char *)file_read(list, NULL)))
		return 0;
	/* the game takes the presets of the list it matched, for its keys and screen mode */
	snprintf(g->preset, sizeof g->preset, "%.*s", (int)strlen(base_name(list)) - 8, base_name(list));
	for (i = 0; i < MAX_DISCS; i++)
		ndisc[i] = -2;          /* not read yet */
	for (line = text; line && *line && !result; line = next)
	{
		char from[300], to[300];
		const char *a, *b;
		int k, j, m;

		next = strchr(line, '\n');
		if (next)
			*next++ = 0;
		line[strcspn(line, "\r")] = 0;
		if (sscanf(line, " copy %d %299s %299[^\n]", &k, from, to) != 3 || from[0] != '$' || to[0] != '$')
			continue;
		if (k < 1 || k > g->ndiscs)
		{
			result = 1;
			break;
		}
		if (ndisc[k - 1] == -2)
			ndisc[k - 1] = disc_files(g, k - 1, &disc[k - 1]);
		if (ndisc[k - 1] < 0)
		{
			snprintf(p->error, sizeof p->error, "The game's disc %d can't be read.", k);
			result = -1;
			break;
		}
		a = from[1] == '.' ? from + 2 : from + 1;
		b = to[1] == '.' ? to + 2 : to + 1;
		for (j = 0; j < ndisc[k - 1]; j++)
		{
			const adfs_file_t *f = &disc[k - 1][j];
			const char *rest = inside(f->path, a);

			if (!rest)
				continue;
			snprintf(path, sizeof path, "%s%s%s", b, b[0] && rest[0] ? "." : "", rest);
			for (m = 0; m < nout && !same_name(out[m].path, path); m++);
			if (m == nout)
			{
				out = realloc(out, (size_t)(nout + 1) * sizeof *out);
				memset(&out[nout++], 0, sizeof *out);
			}
			else
			{
				free((char *)out[m].path);
				free((uint8_t *)out[m].data);
			}
			out[m] = *f;
			out[m].path = strdup(path);
			out[m].data = f->dir ? NULL : memcpy(malloc(f->len ? f->len : 1), f->data, f->len);
		}
	}
	free(text);
	for (i = 0; i < MAX_DISCS; i++)
		if (ndisc[i] >= 0)
			adfs_free(disc[i], ndisc[i]);
	if (!result && !nout)
	{
		snprintf(p->error, sizeof p->error, "The game's presets copy nothing from its discs.");
		result = -1;
	}
	store_game_path(g->id, "", dir, sizeof dir);
	snprintf(hostfs, sizeof hostfs, "%s/hostfs", dir);
	snprintf(list, sizeof list, "%s/installed", dir);
	if (!result)
	{
		remove_tree(hostfs);
		remove_tree(list);
		if (install_hostfs(g->title, "", out, nout, dir, p))
		{
			p->done = 0;    /* it isn't over: the game still plays from its floppies */
			result = -1;
		}
	}
	adfs_free(out, nout);
	if (result)
		return result;

	/* The game starts from the application it installed by its name, whatever its case, or else
	   the first it installed. A space in the name is RISC OS's # here, as it goes on a command line. */
	path[0] = 0;
	d = opendir(hostfs);
	while (d && (e = readdir(d)))
		if (e->d_name[0] != '.')
		{
			const char *x = e->d_name, *y = g->boot;

			for (; *x && *y && (*y == '#' || tolower((unsigned char)*x) == tolower((unsigned char)*y)); x++, y++);
			if (e->d_name[0] == '!' && installed < 2 && (!path[0] || (!*x && !*y)))
				snprintf(path, 64, "%s", e->d_name);
			installed = installed == 2 || (!*x && !*y) ? 2 : 1;
		}
	if (d)
		closedir(d);
	for (i = 0; path[i]; i++)
		if (path[i] == ' ')
			path[i] = '#';
	if (path[0])
		snprintf(g->boot, sizeof g->boot, "%s", path);
	if (!installed || copy_tree(hostfs, list))
	{
		remove_tree(hostfs);
		remove_tree(list);
		return snprintf(p->error, sizeof p->error, "The game's files can't be saved."), -1;
	}
	return 0;
}

/* The game's HostFS folder put back from what its installer installed, when the folder is missing */
void deploy_installed(const char *id)
{
	char installed[1100], hostfs[1100];

	store_game_path(id, "installed", installed, sizeof installed);
	store_game_path(id, "hostfs", hostfs, sizeof hostfs);
	if (is_dir(installed) && !is_dir(hostfs) && copy_tree(installed, hostfs))
		remove_tree(hostfs);
}

int install_classic(const classic_t *c, int desktop, int menu, progress_t *p)
{
	char part[1024], final[1024], path[1100], url[600], support[40];
	adfs_file_t *files;
	int n, i;
	uint8_t *zip;
	size_t len;
	classic_t found;
	game_t g;

	memset(&g, 0, sizeof g);
	snprintf(g.id, sizeof g.id, "%s.part", c->id);
	store_game_path(g.id, "", part, sizeof part);
	store_game_path(c->id, "", final, sizeof final);
	remove_tree(part);
	if (make_dirs(part))
		return fail(p, "The game's folder can't be made.");

	snprintf(p->step, sizeof p->step, "Downloading %s...", c->title);
	p->percent = -1;
	snprintf(path, sizeof path, "%s/package.zip", part);
	snprintf(url, sizeof url, MIRROR "%s", c->mirror);
	if ((!c->mirror[0] || store_fetch(url, path)) && store_fetch(c->url, path))
	{
		remove_tree(part);
		return fail(p, "The download didn't arrive. Check the network connection and try again.");
	}
	zip = file_read(path, &len);
	unlink(path);
	if (zip && !c->boot[0])         /* a package with no Components line: its application is in Apps/<section>/ */
	{
		zipent_t *ents;
		int m = zip_list(zip, len, &ents);

		found = *c;
		for (i = 0; i < m && !found.boot[0]; i++)
		{
			char dir[64], app[40];

			if (sscanf(ents[i].name, "Apps/%63[^/]/%39[^/]/", dir, app) == 2 && app[0] == '!')
			{
				snprintf(found.prefix, sizeof found.prefix, "Apps/%s/", dir);
				snprintf(found.boot, sizeof found.boot, "%s", app);
			}
		}
		free(ents);
		c = &found;
	}
	n = zip ? adfs_from_zip(zip, len, c->prefix, &files) : 0;
	free(zip);
	if (n <= 0)
	{
		remove_tree(part);
		return fail(p, "The download isn't a zip of the game.");
	}
	add_presets(c->id, &files, &n);
	snprintf(g.title, sizeof g.title, "%s", c->title);
	if (!preset_setting(c->id, "support", support, sizeof support))
		support[0] = 0;
	if (layout_files(&g, part, files, n, c->boot, support, p))
	{
		adfs_free(files, n);
		remove_tree(part);
		return -1;
	}
	adfs_free(files, n);

	snprintf(g.info, sizeof g.info, "%s", c->info);
	snprintf(g.boot, sizeof g.boot, "%s", c->boot);
	g.classic = 1;
	g.scale = 2;
	g.autosave = 1;
	g.desktop = desktop;
	g.menu = menu;
	store_save(&g);
	store_pick_icon(&g);

	p->percent = 100;
	snprintf(g.id, sizeof g.id, "%s", c->id);
	remove_tree(final);
	if (rename(part, final))
	{
		remove_tree(part);
		return fail(p, "The game's folder can't be put in place.");
	}
	store_shortcuts(&g);
	p->done = 1;
	return 0;
}
