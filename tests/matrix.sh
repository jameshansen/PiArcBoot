#!/bin/bash
# matrix.sh <packages folder> <floppies folder> <out folder> [jobs]: start every game in the table
# from its first floppy image, with no screen, and keep what its window shows 35 and 65 seconds
# after power on as <out>/<game>-a.png and -b.png, with when the splash came down in <out>/times.txt.
#
# The packages folder holds JASPP's packages as <name>_<version>.zip, and the floppies folder
# holds floppy images from JASPP's release folders, named as JASPP names them (...F1008101.jfd).
# A game's first disc comes from its package if it has one there, and from the floppies otherwise.
PKG=$(realpath "$1"); FLOP=$(realpath "$2"); OUT=$(realpath -m "$3"); JOBS=${4:-8}
BIN=$(realpath "$(dirname "$0")/../build/piarcboot")
mkdir -p "$OUT"; : > "$OUT/times.txt"
one() {
  IFS=$'\t' read -r id boot fdc wp game version <<< "$1"
  H=$(mktemp -d /tmp/pabm-XXXXXX); G="$H/games/$id"; mkdir -p "$G"
  if unzip -l "$PKG/${id}_$version.zip" 2>/dev/null | grep -q "F${game}01\$"; then
    unzip -p "$PKG/${id}_$version.zip" "*/F${game}01" > "$G/disc1.jfd"
  else
    cp "$FLOP"/*"F${game}01.jfd" "$G/disc1.jfd" 2>/dev/null
  fi
  if [ ! -s "$G/disc1.jfd" ]; then echo "$id	no disc image" >> "$OUT/times.txt"; rm -rf "$H"; return; fi
  [ "$boot" = - ] && boot=
  printf 'title=%s\nboot=%s\nfdc=%s\nwp=%s\njaspp=1\nscale=2\ndisc=disc1.jfd\n' "$id" "$boot" "$fdc" "$wp" > "$G/game.ini"
  printf '3500 shot %s\n6500 shot %s\n6501 quit\n' "$OUT/$id-a.png" "$OUT/$id-b.png" > "$H/script"
  T=$(SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy PIARCBOOT_HOME="$H" timeout 600 "$BIN" play "$id" --test "$H/script" 2>&1 | grep "picture shown")
  echo "$id	${T:-never}" >> "$OUT/times.txt"
  rm -rf "$H"
}
export -f one; export PKG FLOP OUT BIN
"$BIN" list | tr '\n' '\0' | xargs -0 -P "$JOBS" -I{} bash -c 'one "$@"' _ {}
sort -o "$OUT/times.txt" "$OUT/times.txt"
echo "$(ls "$OUT" | grep -c -- '-b.png') games ran, $(grep -c 'no disc image' "$OUT/times.txt") had no disc image"
