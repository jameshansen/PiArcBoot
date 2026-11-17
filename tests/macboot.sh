#!/bin/sh
# tests/macboot.sh <PiArcBoot.app>: the Mac application's self-tests, then Asylum installed from
# Classics and booted with no window until its own screen is up, as a check that the machine runs
# on this Mac. The workflow runs it on an Apple silicon Mac and an Intel one.
set -e
A="$1/Contents/MacOS/piarcboot"
H=$(mktemp -d)
PIARCBOOT_HOME=$H "$A" check
PIARCBOOT_HOME=$H "$A" classic Asylum > /dev/null
printf '3000 quit\n' > "$H/script"
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy PIARCBOOT_HOME=$H PIARCBOOT_TRACE=1 "$A" play Asylum --test "$H/script" > "$H/trace.txt" 2>&1
if ! grep "picture shown" "$H/trace.txt"; then
	tail -20 "$H/trace.txt"
	echo "Asylum didn't reach its own screen" >&2
	exit 1
fi
