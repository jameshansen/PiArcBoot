#!/bin/sh
# trace.sh <rom> <disc image> shift|<application> [fdc]: boot a game with no window and print when
# it sets screen modes and when the floppy is read and stops. This is what the splash screen's
# timing was tuned from.
ROM=$1; IMG=$2; HOW=$3; FDC=${4:-0}
S=$(mktemp)
if [ "$HOW" = shift ]; then
  printf 'disc 0 %s\nkey shift down\nwait 600\nkey shift up\nwait 3400\n' "$IMG" > "$S"
else
  printf 'disc 0 %s\nwait 1900\nkey f12 down\nwait 10\nkey f12 up\nwait 100\ntype *Filer_OpenDir adfs::0.$\\n\nwait 100\ntype *Filer_Run adfs::0.$.%s\\n\nwait 50\ntype \\n\nwait 3000\n' "$IMG" "$HOW" > "$S"
fi
PIARCBOOT_TRACE=1 "$(dirname "$0")/../build/piarcboot" test "$ROM" "$S" "$FDC" 2>&1
rm -f "$S"
