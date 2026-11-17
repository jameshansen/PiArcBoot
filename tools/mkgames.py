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
    if preset(gid, "patch"):
        n.append("No manual check")
    return ", ".join(n)


# Why each package in JASPP's feed that the JASPP tab leaves out is left out, by group, as tried
# on ROO311 in PiArcBoot. A package JASPP adds later shows as not checked yet until it has a line.
LEFT_OUT_GROUPS = [
    ("tools", "Not games", "Tools and add-ons in JASPP's feed."),
    ("later", "Versions for later machines",
     "Releases for the StrongARM, the Risc PC, 32-bit RISC OS or CD, packaged as applications for those machines "
     "with no floppy image to boot from."),
    ("noimage", "No floppy image",
     "Packaged as applications, with no floppy image in the package or in the game's release folder on JASPP's "
     "server, so there is nothing to boot."),
    ("broken", "Not playing yet", "They have floppy images, but don't play on ROO311 yet."),
    ("new", "Not checked yet", "New to JASPP's feed since this list was made."),
]
LEFT_OUT = {
    "ADFFS": ("tools", "ADFFS", "A floppy disc emulator for RISC OS"),
    "CDFaker": ("tools", "CDFaker", "Mounts CD images"),
    "GEKonfig": ("tools", "GEKonfig", "Data the GEK games share"),
    "HDFFS": ("tools", "HDFFS", "A filing system for hard disc images"),
    "HoledOut-Courses": ("tools", "Courses for Holed Out Compendium", "Extra courses for Holed Out Compendium, which is in the list"),
    "PartMgr": ("tools", "Partition Manager", "A disc partitioning tool"),
    "pi-topUtils": ("tools", "pi-top Utilities", "Utilities for the pi-top"),
    "Botkiller-[32bit]": ("later", "Botkiller [32bit version]", "For 32-bit RISC OS"),
    "Burn-Out-[SA]": ("later", "Burn'Out [SA version]", "For the StrongARM"),
    "Chuck-Rock-[SA]": ("later", "Chuck Rock [SA version]", "For the StrongARM. The original Chuck Rock is in the list"),
    "Drifter-[SA]": ("later", "Drifter [SA version]", "For the StrongARM, installed to a hard disc first. The original Drifter is in the list"),
    "Dune-II-[CD]": ("later", "Dune II - Battle for Arrakis [CD version]", "On CD"),
    "Exodus-[32bit]": ("later", "Exodus [32bit version]", "For 32-bit RISC OS"),
    "Gods-[RPC]": ("later", "Gods [RPC version]", "For the Risc PC. The original Gods is in the list"),
    "Iron-Lord-[32bit]": ("later", "Iron Lord [32bit version]", "For 32-bit RISC OS"),
    "James-Pond-[SA]": ("later", "James Pond [SA version]", "For the StrongARM. The original James Pond is in the list"),
    "Lemmings-2-The-Tribes-[SA]": ("later", "Lemmings 2: The Tribes [SA version]", "For the StrongARM. The original Lemmings 2 is in the list"),
    "MicroDrive-32M-[32bit]": ("later", "MicroDrive 32M", "For 32-bit RISC OS. The original MicroDrive is in the list"),
    "Populous-[RPC]": ("later", "Populous [RPC version]", "For the Risc PC, and can't find its !System on ROO311. The original Populous is in the list"),
    "Pushy-II-[32bit]": ("later", "Pushy II [32bit version]", "For 32-bit RISC OS"),
    "SimCity2000-[A5000]": ("later", "SimCity 2000 [A5000 version]", "Packaged to install on a hard disc"),
    "SimCity2000-[RPC]": ("later", "SimCity 2000 [RPC version]", "For the Risc PC"),
    "SunBurst-[32bit]": ("later", "SunBurst [32bit version]", "For 32-bit RISC OS"),
    "Twinworld-[32bit]": ("later", "Twinworld [32bit version]", "For 32-bit RISC OS"),
    "Wolfenstein-3D-[32bit]": ("later", "Wolfenstein 3D [32bit version]", "For 32-bit RISC OS"),
    "Zool-[32bit]": ("later", "Zool - Ninja of the 'Nth' Dimension [32bit version]",
                     "For 32-bit RISC OS. Started from the desktop it shows nothing, and from the command line it "
                     "takes 2 minutes to reach its title"),
    "Asylum-[Acorn-Arcade]": ("noimage", "Asylum", "The Classics tab has Asylum"),
    "DropShaft": ("noimage", "DropShaft", ""),
    "Great-Giana-Sisters": ("noimage", "The Great Giana Sisters", ""),
    "Hamsters": ("noimage", "Hamsters", ""),
    "Pocket-Money-Demo": ("noimage", "Pocket Money Demo", ""),
    "The-Swarm": ("noimage", "The Swarm", ""),
    "World-Champ-Boxing-Manager": ("noimage", "World Championship Boxing Manager", ""),
    "Adventure-Collection": ("broken", "The Adventure Collection", "COPS and Plague Planet together, which are both in the list"),
    "ArcPinball": ("broken", "ArcPinball", "Stops with \"Attempt to use badly nested error handler\" over and over"),
    "Ballarena": ("broken", "Ballarena", "Stays on a blank screen"),
    "Brutal-Horse-Power": ("broken", "Brutal Horse Power", "Starts with a hard disc installer that needs a !System, which hasn't been tried"),
    "Bubble-Fair": ("broken", "Bubble Fair", "Asks before it takes over the machine, and then never shows"),
    "Cartoon-Line-part-one": ("broken", "Cartoon Line part one", "Stops with an abort on data transfer after its first screen"),
    "Command-Ship": ("broken", "Command Ship", "Fails its disc check, asking for the original program disc, even with the disc write protected"),
    "Emotions-Search-for-Humanity": ("broken", "Emotions - Search for Humanity", "Its discs can't be read with either floppy controller"),
    "Fine-Racer": ("broken", "Fine Racer", "Asks before it takes over the machine, and then never shows"),
    "Flying-High": ("broken", "Flying High", "Its discs can't be read with either floppy controller"),
    "Formula-Two-Thousand": ("broken", "Formula Two Thousand", "Fails its disc check, asking for the original program disc, even with the disc write protected"),
    "Grievous-Bodily-ARM": ("broken", "Grievous Bodily 'ARM", "Stops with an abort on data transfer behind its title"),
    "Holed-Out": ("broken", "Holed Out!!", "Asks for its disc to be write protected, even when it is. Holed Out Compendium is in the list"),
    "KerBang": ("broken", "KerBang!", "Stops with \"Application may have gone wrong\""),
    "Minotaur": ("broken", "Minotaur", "Stays on a blank screen"),
    "Tactic": ("broken", "Tactic", "Stays on a blank screen"),
}


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
         "- **1.6MB disc**: its disc is a high density one, so the machine becomes an A5000 to read it.",
         "- **No manual check**: it skips the question it asks from its manual, or for another game's disc,"
         " with the change to its code that ADFFS, JASPP's floppy disc emulator, makes.", "",
         "## JASPP", "",
         "Released by [JASPP](https://www.jaspp.org.uk), the Archimedes Software Preservation Project, with their "
         "publishers' permission, and listed here once they start on ROO311.", "",
         "| Game | Year | Publisher | Discs | |", "|---|---|---|---|---|"]
for gid, title, info, fdc, discs in jaspp:
    year, _, pub = info.partition(", ")
    if not re.fullmatch(r"\d{4}", year):
        year, pub = "", info
    lines.append("| %s | %s | %s | %s | %s |" % (cell(title), year, cell(pub), discs, notes(gid, fdc)))

hidden = re.findall(r'^\t"([^"]+)",$', cat[cat.index("catalog_hidden"):], re.M)
lines += ["", "## JASPP: left out", "",
          "The %d packages in JASPP's feed that the JASPP tab leaves out, and why." % len(hidden)]
for group, head, text in LEFT_OUT_GROUPS:
    rows = sorted((LEFT_OUT[g][1], LEFT_OUT[g][2]) if g in LEFT_OUT else (g, "")
                  for g in hidden if LEFT_OUT.get(g, ("new",))[0] == group)
    if not rows:
        continue
    lines += ["", "### " + head, "", text, "", "| Package | Why |", "|---|---|"]
    for title, why in rows:
        lines.append("| %s | %s |" % (cell(title), cell(why)))

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
