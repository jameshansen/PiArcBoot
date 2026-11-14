#!/usr/bin/env python3
"""Make src/catalog.h, the table of JASPP games PiArcBoot knows how to start.

    python tools/mkcatalog.py <feed index> <packages folder> <launch list> <ROO311 README.md> <release counts>

It joins 5 sources:
  - the JASPP PackMan feed index (package name, version, size, and a description line of the form
    "Title (Year) (Publisher) Fnnnnnnn", which ends in JASPP's number for the game),
  - the packages themselves, for the floppy images each one holds (files named F and 7 digits)
    and the size of the disc set those belong to (in each image's header),
  - the launch list from ROO311's game tests, one line a game: package name, "shift" (the disc
    boots with Shift held) or "run", the application to run, the floppy controller (0 the 1772,
    1 the 82C711) and whether the disc has to be write protected,
  - the games table in ROO311's README, which says how each game fared on that ROM,
  - a count of the floppy images in each game's release folder on JASPP's server, a line a game
    of package name, game number and count, as tests/floppies.sh makes it.

A game goes in the table when the README has it green, or names it among the extra packages that
reach the same screen as RISC OS 3.11. Those that play in the desktop are shown cropped to their
windows, as their presets say (presets/README.md). A game with no floppy image in its package or its release folder is left out too, since
there is nothing to boot it from. Every other package in the feed is listed by name only, so the
launcher can tell a package it has left out from one JASPP has added since.
"""
import glob
import gzip
import os
import re
import struct
import sys
import zipfile

MANUAL = {
    "Battle Tank": "Battle-Tanks",
    "Bobby Blockhead vs The Dark Planet": "Bobby-Blockhead-vs-Dark-Planet",
    "Dune II - Battle for Arrakis [CD version]": "Dune-II-[CD]",
    "The Exotic Adventures of Sylvia Layne": "Exotic-Adventures-Sylvia-Layne",
    "Jahangir Khan World Championship Squash": "Jahangir-Khan-Squash",
    "Zool - Ninja of the 'Nth' Dimension [32bit version]": "Zool-[32bit]",
}
EXTRA = ["Adventures-classic-compilation", "Champions", "COPS", "Killerbugs", "Plague-Planet", "SpySnatcher",
         "World-Class-Leaderboard"]


def norm(s):
    s = re.sub(r"\[.*?\]|\(.*?\)", "", s.lower().replace("&", "and"))
    return re.sub(r"[^a-z0-9]", "", re.sub(r"^the ", "", s.strip()))


def tag(s):
    m = re.search(r"\[(.*?)\]", s)
    return re.sub(r"[^a-z0-9]", "", m.group(1).lower().replace("version", "")) if m else ""


def cstr(s):
    return '"' + s.encode("latin-1", "replace").decode("latin-1").replace("\\", "\\\\").replace('"', '\\"') + '"'


def main():
    feed_file, pkg_dir, launch_file, readme, release_file = sys.argv[1:6]
    release = {}
    for line in open(release_file, encoding="utf-8"):
        p = line.split("\t")
        if len(p) >= 3:
            release[p[0]] = int(p[2])
    feed = {}
    for block in re.split(r"\n\s*\n", open(feed_file, encoding="latin-1").read()):
        m = re.search(r"^Package:\s*(.+)$", block, re.M)
        if not m:
            continue
        d = {k: (re.search(r"^%s:\s*(.*)$" % k, block, re.M) or [0, ""])[1].strip()
             for k in ("Version", "Size", "Description", "Section")}
        feed[m.group(1).strip()] = d

    launch = {}
    for line in open(launch_file, encoding="utf-8"):
        p = line.rstrip("\n").split("\t")
        if len(p) >= 5:
            launch[p[0]] = p[1:]

    # the README's table: (status, title, year, how it starts)
    rows = re.findall(r"^\| (\S+) \| (.+?) \| (\d{4}) \| (.*?) \| .*? \|$", open(readme, encoding="utf-8").read(), re.M)
    chosen = {}
    for status, title, year, starts in rows:
        name = MANUAL.get(title)
        if not name:
            same = [n for n in feed if norm(n) == norm(title)]
            exact = [n for n in same if tag(n) == tag(title)]
            name = (exact or (same if len(same) == 1 else [n for n in same if not tag(n)]) or [None])[0]
        assert name in feed, title
        if status == "\U0001F7E2":
            chosen[name] = (title, year)
    for name in EXTRA:
        d = feed[name]["Description"]
        chosen[name] = (re.sub(r"\s*\(.*$", "", d), (re.search(r"\((\d{4})\)", d) or [0, ""])[1])

    out, no_images = [], []
    for name in sorted(chosen, key=lambda n: norm(chosen[n][0])):
        title, year = chosen[name]
        how, app, fdc, wp = launch[name][:4]
        assert how in ("shift", "run"), name
        d = feed[name]
        pub = re.findall(r"\(([^()]*)\)", d["Description"])
        pub = pub[1] if len(pub) > 1 else ""
        if pub.isupper():
            pub = pub.title()
        discs, size_of_set = 0, 0
        for z in glob.glob(os.path.join(pkg_dir, glob.escape(name) + "_*.zip")):
            zf = zipfile.ZipFile(z)
            for n in zf.namelist():
                if re.fullmatch(r"F\d{7}", n.split("/")[-1]):
                    b = zf.read(n)
                    b = gzip.GzipFile(fileobj=__import__("io").BytesIO(b)).read(16) if b[:2] == b"\x1f\x8b" else b[:16]
                    if b[:4] == b"JFDI":
                        discs += 1
                        size_of_set = max(size_of_set, struct.unpack_from("<I", b, 12)[0] >> 16)
        if not discs and not release.get(name, 0):
            no_images.append(name)          # nothing to boot it from
            continue
        info = ", ".join(x for x in (year, pub) if x)
        gid = re.search(r"\bF(\d{5})\d{2}\b", d["Description"])
        assert gid, name
        out.append("\t{ %s, %s, %s, %s, %s, %s, %s, %s, %s, %d, %d, %d }," % (
            cstr(name), cstr(title), cstr(info), cstr(d["Version"]), cstr("" if how == "shift" else app),
            gid.group(1), d["Size"] or "0", fdc, wp, discs, max(size_of_set, discs, release.get(name, 0)),
            release.get(name, 0)))

    hidden = sorted(n for n in feed if n not in chosen or n in no_images)
    text = """/* Made by tools/mkcatalog.py. Do not edit.

   The JASPP games PiArcBoot starts, from the JASPP package feed and ROO311's game tests. */
typedef struct
{
	const char *id;         /* the package's name in the JASPP feed */
	const char *title;
	const char *info;       /* year and publisher */
	const char *version;    /* the package's version when this table was made */
	const char *boot;       /* the application to run, or "" for a disc that boots with Shift held */
	unsigned game;          /* JASPP's number for the game: its floppy images are F<game><disc> */
	unsigned size;          /* the package's size in bytes */
	unsigned char fdc;      /* 1 for the 82C711 floppy controller (1.6MB discs), 0 for the 1772 */
	unsigned char wp;       /* the disc has to be write protected */
	unsigned char discs;    /* floppy images in the package */
	unsigned char set;      /* floppy images in the whole game */
	unsigned char release;  /* floppy images in the game's release folder on JASPP's server */
} catalog_t;

static const catalog_t catalog[] = {
%s
};

/* Packages in the feed that are left out: tools, later versions for other machines, games that
   don't start on ROO311 yet, and games with no floppy image to boot from */
static const char *const catalog_hidden[] = {
%s
};
""" % ("\n".join(out), "\n".join("\t%s," % cstr(n) for n in hidden))
    open(os.path.join(os.path.dirname(__file__), "..", "src", "catalog.h"), "w", newline="\n").write(text)
    counts = [tuple(int(x) for x in re.findall(r"(\d+), (\d+), \d+ \},$", l)[0]) for l in out]
    full = sum(1 for d, n in counts if d and d >= n)
    none = sum(1 for d, n in counts if not d)
    print(f"{len(out)} games: {full} with every floppy image in the package, {len(out) - full - none} with some, "
          f"{none} with none. {len(hidden)} packages left out, {len(no_images)} of them for having no floppy image")


if __name__ == "__main__":
    main()
