#!/bin/sh
# Installs PiArcBoot on a Raspberry Pi or a Linux PC from the terminal:
#
#   curl -fsSL https://github.com/jameshansen/PiArcBoot/releases/latest/download/install.sh | sh
#
# It downloads the installer for this computer from the release it came with and runs it. With no
# desktop to show the installer on, as over SSH, PiArcBoot is installed or updated with the usual
# settings. The release workflow writes the release's tag in place of {TAG}.
main() {
  set -e
  tag={TAG}
  url=${PIARCBOOT_RELEASE:-https://github.com/jameshansen/PiArcBoot/releases/download/$tag}
  case $(uname -m) in
    aarch64|arm64) build=RaspberryPi ;;
    x86_64|amd64) build=Linux-x86-64 ;;
    armv*) echo "PiArcBoot needs a 64-bit system, such as 64-bit Raspberry Pi OS." >&2; exit 1 ;;
    *) echo "There is no PiArcBoot for $(uname -m)." >&2; exit 1 ;;
  esac
  if ! { ldconfig -p 2>/dev/null || /sbin/ldconfig -p 2>/dev/null; } | grep -q 'libSDL2-2\.0\.so'; then
    echo "PiArcBoot needs the SDL2 library. Install it with:  sudo apt install libsdl2-2.0-0" >&2
    exit 1
  fi
  if ! command -v unzip >/dev/null && ! command -v python3 >/dev/null; then
    echo "Unpacking the installer needs unzip. Install it with:  sudo apt install unzip" >&2
    exit 1
  fi
  dir=$(mktemp -d)
  trap 'rm -rf "$dir"' EXIT
  echo "Downloading PiArcBoot-Installer-$build.zip"
  curl -fsSL -o "$dir/installer.zip" "$url/PiArcBoot-Installer-$build.zip"
  if command -v unzip >/dev/null; then
    unzip -q "$dir/installer.zip" -d "$dir" 2>/dev/null || ok=no
  else
    python3 -c 'import sys, zipfile; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])' "$dir/installer.zip" "$dir" 2>/dev/null || ok=no
  fi
  if [ "${ok:-}" = no ] || [ ! -f "$dir/Install PiArcBoot" ]; then
    echo "The installer didn't download whole. Try again." >&2
    exit 1
  fi
  chmod +x "$dir/Install PiArcBoot"
  "$dir/Install PiArcBoot" "$@" < /dev/null
}

# All of it is read before any of it runs, so a download cut short does nothing
main "$@"
