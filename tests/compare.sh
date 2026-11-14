#!/bin/bash
# compare.sh <packages> <floppies> <out> <game>...: boot the named games in PiArcBoot and keep what
# the window shows 65 and 95 seconds after power on, as matrix.sh does for the whole table but at
# later times, for looking closer at games that were slow to show anything.
PKG=$(realpath "$1"); FLOP=$(realpath "$2"); OUT=$(realpath -m "$3"); shift 3
BIN=$(realpath "$(dirname "$0")/../build/piarcboot")
mkdir -p "$OUT"
one() {
  IFS=$'\t' read -r id boot fdc wp game version <<< "$("$BIN" list | grep -F "$1	" | head -1)"
  [ -n "$id" ] || { echo "$1: not in the table"; return; }
  H=$(mktemp -d /tmp/pabc-XXXXXX); G="$H/games/$id"; mkdir -p "$G"
  if unzip -l "$PKG/${id}_$version.zip" 2>/dev/null | grep -q "F${game}01\$"; then
    unzip -p "$PKG/${id}_$version.zip" "*/F${game}01" > "$G/disc1.jfd"
  else
    cp "$FLOP"/*"F${game}01.jfd" "$G/disc1.jfd" 2>/dev/null
  fi
  [ "$boot" = - ] && boot=
  printf 'title=%s\nboot=%s\nfdc=%s\nwp=%s\njaspp=1\nscale=2\ndisc=disc1.jfd\n' "$id" "$boot" "$fdc" "$wp" > "$G/game.ini"
  printf '6500 shot %s\n9500 shot %s\n9501 quit\n' "$OUT/$id-b.png" "$OUT/$id-c.png" > "$H/script"
  SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy PIARCBOOT_HOME="$H" timeout 900 "$BIN" play "$id" --test "$H/script" 2>&1 | grep "picture shown" | sed "s/^/$id: /"
  rm -rf "$H"
}
export -f one; export PKG FLOP OUT BIN
printf '%s\0' "$@" | xargs -0 -P 12 -I{} bash -c 'one "$@"' _ {}
