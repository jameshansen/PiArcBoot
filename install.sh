#!/bin/bash
# PiArcBoot: installs PiArcBoot on Raspberry Pi OS and other Linux desktops.
#
# Run it from the desktop, from the folder it came in. PiArcBoot goes into ~/Games/PiArcBoot,
# with an entry in the Games menu and a shortcut on the desktop. Run it again to reinstall or
# remove. In a copy of the source with no program built for this machine, it builds one first,
# which needs gcc, make and the SDL2 and zlib development packages.
set -u
: "${HOME:?}"

TITLE="PiArcBoot"
HERE=$(dirname "$(readlink -f "$0")")
ICON="$HERE/icon.png"
DEST="$HOME/Games/PiArcBoot"
# The menu entry carries the name the launcher's window goes by, so the taskbar shows its icon.
# The desktop shortcut is the same file.
MENU="${XDG_DATA_HOME:-$HOME/.local/share}/applications/piarcboot.desktop"
SHORTCUT="$(xdg-user-dir DESKTOP 2>/dev/null || echo "$HOME/Desktop")/piarcboot.desktop"

if ! command -v zenity >/dev/null || [ -z "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ]; then
  echo "Run this installer from the desktop. It needs zenity (sudo apt install zenity)." >&2
  exit 1
fi

# A page with up to three buttons, left to right: --cancel-label, --ok-label, --extra-button.
# OK returns 0. The extra button prints its label and returns 1, like Cancel.
page() { zenity --question --title="$TITLE" --width=560 --icon="$ICON" "$@" 2>/dev/null; }
note() { zenity --info --title="$TITLE" --width=560 --icon="$ICON" --text="$1" 2>/dev/null; }
fail() { zenity --error --title="$TITLE" --width=560 --no-markup --text="$1" 2>/dev/null; exit 1; }
busy() { zenity --progress --title="$TITLE" --width=560 --no-cancel --auto-close "$@" 2>/dev/null; }

for f in ROM311 icon.png icons LICENSE; do
  [ -e "$HERE/$f" ] || fail "$f is missing from beside install.sh. Download the whole package again."
done
command -v curl >/dev/null ||
  fail "PiArcBoot fetches games with curl, which isn't installed. Install it with:

sudo apt install curl"

if [ -e "$DEST/piarcboot" ]; then
  choice=$(page --text="<b>PiArcBoot is already installed.</b>\n\nReinstall sets it up again. Remove takes PiArcBoot and its shortcuts away, and leaves the games you installed with it, which each game's own Settings can remove." \
    --cancel-label="Cancel" --ok-label="Reinstall" --extra-button="Remove")
  status=$?
  if [ "$choice" = "Remove" ]; then
    rm -rf "$DEST" "$MENU" "$SHORTCUT"
    note "PiArcBoot has been removed. The games you installed with it are still in ~/.local/share/piarcboot."
    exit 0
  fi
  [ "$status" -eq 0 ] || exit 0
else
  page --text="<b>Welcome to the PiArcBoot Installer</b>\n\nPiArcBoot installs Acorn Archimedes games from JASPP and boots each one straight into the game. This installer puts PiArcBoot in the Games folder in your home folder, adds it to the Games menu and places a shortcut on the desktop.\n\nPiArcBoot takes about 4 MB, and each game under 1 MB a disc." \
    --cancel-label="Cancel" --ok-label="Install" || exit 0
fi

# The program that came in the package, if it runs here; otherwise one built from the source
BIN="$HERE/piarcboot"
if ! { [ -x "$BIN" ] && "$BIN" check >/dev/null 2>&1; }; then
  if [ -x "$BIN" ] && ! ldconfig -p 2>/dev/null | grep -q 'libSDL2-2\.0\.so'; then
    fail "PiArcBoot needs the SDL2 library, which isn't installed. Install it with:

sudo apt install libsdl2-2.0-0"
  fi
  [ -f "$HERE/Makefile" ] ||
    fail "This copy of PiArcBoot was built for another kind of computer. Download PiArcBoot.tar.gz for a Raspberry Pi, or PiArcBoot-Linux-x86-64.tar.gz for a PC."
  log=$(mktemp)
  make -C "$HERE" -j"$(nproc 2>/dev/null || echo 2)" >"$log" 2>&1 | busy --pulsate --text="Building PiArcBoot for this computer..."
  [ -x "$HERE/build/piarcboot" ] || fail "PiArcBoot didn't build. It needs gcc, make, libsdl2-dev and zlib1g-dev.

$(tail -5 "$log")"
  rm -f "$log"
  BIN="$HERE/build/piarcboot"
fi
GUI="$(dirname "$BIN")"         # ROOGUI's library and its licence are beside the program

install_files() (
  set -e
  mkdir -p "$DEST" "$(dirname "$MENU")" "$(dirname "$SHORTCUT")"
  rm -rf "$DEST/icons"
  echo "# Copying PiArcBoot..."
  cp "$BIN" "$GUI/libroogui.so" "$DEST/"
  [ ! -f "$GUI/LICENSE-ROOGUI" ] || cp "$GUI/LICENSE-ROOGUI" "$DEST/"
  cp -r "$HERE/ROM311" "$HERE/icon.png" "$HERE/icons" "$HERE/LICENSE" "$DEST/"
  echo 80
  echo "# Adding the menu entry and the desktop shortcut..."
  cat >"$MENU" <<EOF
[Desktop Entry]
Type=Application
Name=PiArcBoot
Comment=Acorn Archimedes games, booted straight from their disc images
Exec="$DEST/piarcboot"
Icon=$DEST/icon.png
Terminal=false
Categories=Game;
StartupWMClass=piarcboot
EOF
  cp "$MENU" "$SHORTCUT"
  chmod +x "$SHORTCUT"
)

log=$(mktemp)
{ install_files 2>"$log"; status=$?; echo 100; exit $status; } | busy --percentage=0 --text="Installing PiArcBoot..."
[ "${PIPESTATUS[0]}" -eq 0 ] || fail "The install did not finish.

$(tail -5 "$log")"
rm -f "$log"

page --text="<b>PiArcBoot is installed.</b>\n\nIt is in the Games menu and on the desktop. Choose a game, press Install, then Play." \
  --cancel-label="Close" --ok-label="Play Now" && setsid "$DEST/piarcboot" >/dev/null 2>&1 &
