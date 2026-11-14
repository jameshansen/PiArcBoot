#!/bin/bash
# floppies.sh <out file>: for every game in the table, count the floppy images in its release
# folder on JASPP's server. Each line of the result is the package name, the game's number and
# the count, which is what tools/mkcatalog.py takes as its release counts. It asks for one small
# page a game, a third of a second apart.
OUT=$1
BIN=$(realpath "$(dirname "$0")/../build/piarcboot")
: > "$OUT"
"$BIN" list | while IFS=$'\t' read -r id boot fdc wp game version; do
  n=$(curl -fsL --connect-timeout 20 --max-time 60 "https://forums.jaspp.org.uk/release/F$game/Discs/" | grep -o 'href="[^"]*\.jfd"' | wc -l)
  printf '%s\t%s\t%s\n' "$id" "$game" "$n" >> "$OUT"
  sleep 0.3
done
