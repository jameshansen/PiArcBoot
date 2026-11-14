#!/bin/sh
# Builds PiArcBoot for 64-bit Windows with zig (https://ziglang.org), from Git Bash on Windows or
# from any other machine zig runs on. The result is build/win/PiArcBoot, a folder to copy anywhere.
#
#   sh tools/build-windows.sh <SDL2 folder> <zlib folder>
#
# ROOGUI, the library that draws the windows, is looked for in a folder called roogui here, then
# in ROOGUI beside this one. Its own script builds it, and roogui.dll goes beside the program.
#
# <SDL2 folder> is the x86_64-w64-mingw32 folder of SDL2's "devel mingw" package, from
# https://github.com/libsdl-org/SDL/releases. <zlib folder> holds zlib.h, zconf.h, zlib1.dll and
# an import library for it, libz.dll.a. Git for Windows has the DLL (mingw64/bin/zlib1.dll), and
# the import library comes from a list of the 8 functions PiArcBoot calls:
#
#   printf 'LIBRARY zlib1.dll\nEXPORTS\ninflateInit2_\ninflate\ninflateEnd\ninflateReset\ncompress2\ncompressBound\nuncompress\ncrc32\n' > zlib1.def
#   zig dlltool -d zlib1.def -l libz.dll.a -m i386:x86-64
#
# VERSION, when set, is what the About tab shows; otherwise it is git's name for the commit.
set -e
SDL=$1; ZLIB=$2
HERE=$(cd "$(dirname "$0")/.." && pwd)
VERSION=${VERSION:-$(git -C "$HERE" describe --tags --always --dirty 2>/dev/null | sed 's/^v//')}
OUT="$HERE/build/win/PiArcBoot"; OBJ="$HERE/build/win/obj"
GUI="$HERE/roogui"; [ -d "$GUI" ] || GUI="$HERE/../ROOGUI"
mkdir -p "$OUT" "$OBJ"
objs=
sh "$GUI/tools/build-windows.sh" "$SDL" "$ZLIB" >/dev/null
# The machine is ArcEm's, in arcem/; the files not listed here are parts of those that are
ARCEM="armcopro.c armemu.c arminit.c armsupp.c eventq.c hostfs.c arch/archio.c arch/armarc.c arch/cp15.c
  arch/displaydev.c arch/extnrom.c arch/filecommon.c arch/filewin.c arch/i2c.c arch/keyboard.c arch/newsound.c"
newest=$(find "$HERE/src" "$HERE/arcem" "$GUI/src" \( -name '*.h' -o -name 'armemu*.c' -o -name 'fastmap.c' -o -name 'stddisplaydev.c' \) -print0 | xargs -0 ls -t | head -1)
for f in $(for a in $ARCEM; do echo "arcem/$a"; done) $(cd "$HERE" && ls src/*.c); do
  o="$OBJ/$(basename "$f" .c).o"
  case "$f" in
    arcem/*|src/screen.c) opt="-O3" ;;
    *) opt="-O2" ;;
  esac
  case "$f" in
    arcem/*) warn="-Wall -W -Wno-unused-parameter" ;;
    *) warn="-Wall -Wno-format-truncation" ;;
  esac
  if [ ! -f "$o" ] || [ "$HERE/$f" -nt "$o" ] || [ "$newest" -nt "$o" ]; then
    zig cc -target x86_64-windows-gnu -std=gnu17 $opt $warn -DSOUND_SUPPORT -DHOSTFS_SUPPORT -DEXTNROM_SUPPORT -DROOGUI_DLL ${VERSION:+"-DPIARCBOOT_VERSION=\"$VERSION\""} -I"$HERE/src" -I"$GUI/src" \
      -I"$SDL/include/SDL2" -I"$ZLIB" -c "$HERE/$f" -o "$o"
  fi
  objs="$objs $(basename "$o")"
done
cd "$OBJ"       # the objects are named without their folder, which may have a space in it
zig cc -target x86_64-windows-gnu -o "$OUT/piarcboot.exe" $objs -lmingw32 \
  "$SDL/lib/libSDL2main.a" "$SDL/lib/libSDL2.dll.a" "$ZLIB/libz.dll.a" "$GUI/build/win/libroogui.dll.a" -lcomdlg32 -lshell32 -Wl,--subsystem,windows
cp "$SDL/bin/SDL2.dll" "$ZLIB/zlib1.dll" "$GUI/build/win/roogui.dll" "$OUT/"
cp "$GUI/LICENSE" "$OUT/LICENSE-ROOGUI"
rm -rf "$OUT/icons"
cp -r "$HERE/ROM311" "$HERE/icon.png" "$HERE/icons" "$HERE/LICENSE" "$OUT/"
echo "built $OUT"
