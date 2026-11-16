#!/usr/bin/env python3
"""Writes GAMES.md, every game PiArcBoot's tabs list: the JASPP table from src/catalog.h, and the
Classics as the program itself lists them (piarcboot classic, which fetches PackMan's indexes), with
what presets/ says about each.

    python3 tools/mkgames.py <piarcboot>"""
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
piarcboot = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "build", "piarcboot"))


def preset(gid, key):
    path = os.path.join(HERE, "presets", gid + ".ini")
    if not os.path.exists(path):
        return None
    m = re.search(r"^%s=(.*)$" % key, open(path, encoding="utf-8").read(), re.M)
    return m.group(1).strip() if m else None


def notes(gid, fdc=0, support=False):
    n = []
    if os.path.exists(os.path.join(HERE, "presets", gid + ".install")) or preset(gid, "apps"):
        n.append("Hard disc install")
    if preset(gid, "crop") == "1":
        n.append("Crop to Desktop Window")
    if fdc == "1":
        n.append("1.6MB disc")
    if support or preset(gid, "support"):
        n.append("Support pack")
    return ", ".join(n)


def cell(text):
    return text.replace("|", "\\|")


cat = open(os.path.join(HERE, "src", "catalog.h"), encoding="utf-8").read()
jaspp = re.findall(r'^\t\{ "([^"]+)", "([^"]+)", "([^"]*)", "[^"]*", "[^"]*", \d+, \d+, (\d), \d, \d+, (\d+), \d+ \}', cat, re.M)
with tempfile.TemporaryDirectory() as home:
    out = subprocess.run([piarcboot, "classic"], capture_output=True, encoding="latin-1",
                         env=dict(os.environ, PIARCBOOT_HOME=home), check=True).stdout
classics = [line.split("\t") for line in out.splitlines() if line.count("\t") >= 6]

lines = ["# Games", "",
         "Every game PiArcBoot lists, %d in its JASPP tab and %d in Classics. PiArcBoot contains none of them. Install "
         "downloads each one, and an installed game plays offline." % (len(jaspp), len(classics)), "",
         "- **Hard disc install**: its installer's files are copied onto a folder the machine reads directly, so it "
         "starts with no discs to swap.",
         "- **Crop to Desktop Window**: it plays in the RISC OS desktop and is shown cropped to its windows.",
         "- **Support pack**: it installs with ROO311's support pack, the newer Window Manager and the Toolbox it "
         "needs.",
         "- **1.6MB disc**: its disc is a high density one, so the machine becomes an A5000 to read it.", "",
         "## JASPP", "",
         "Released by [JASPP](https://www.jaspp.org.uk), the Archimedes Software Preservation Project, with their "
         "publishers' permission, and listed here once they start on ROO311 as they do on RISC OS 3.11.", "",
         "| Game | Year | Publisher | Discs | |", "|---|---|---|---|---|"]
for gid, title, info, fdc, discs in jaspp:
    year, _, pub = info.partition(", ")
    if not re.fullmatch(r"\d{4}", year):
        year, pub = "", info
    lines.append("| %s | %s | %s | %s | %s |" % (cell(title), year, cell(pub), discs, notes(gid, fdc)))

for section, head, text in (
        ("0", "Classics: free games",
         "Freely distributable games that come as RISC OS applications, former commercial games their authors and "
         "publishers let Acorn Arcade give away, mirrored in `distributable-games/`, and Star Fighter 3000's "
         "release 3.21 from its own site. Install makes floppy images of them, or a folder the machine reads "
         "directly."),
        ("1", "Classics: PackMan",
         "Desktop games from the indexes of PackMan, RISC OS Open's package manager, that run on RISC OS 3.11. The "
         "list follows the indexes, so a game added to them appears in the tab by itself.")):
    rows = [c for c in classics if c[6] == section]
    lines += ["", "## " + head, "", text, "",
              "| Game | %s | |" % ("Credits" if section == "0" else "What it is"), "|---|---|---|"]
    for c in rows:
        lines.append("| %s | %s | %s |" % (cell(c[4]), cell(c[5]), notes(c[0])))

lines += ["", "This list is made by `tools/mkgames.py` from the program's own lists.", ""]
open(os.path.join(HERE, "GAMES.md"), "w", encoding="utf-8", newline="\n").write("\n".join(lines))
print("GAMES.md: %d JASPP games, %d Classics" % (len(jaspp), len(classics)))
