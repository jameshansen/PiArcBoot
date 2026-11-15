#!/bin/bash
# savestate.sh <script> [cs] [fdc]: a save state put back carries on exactly as the machine it was
# taken from. The first run plays the piarcboot test script, saves the machine's state at its end
# and runs cs centiseconds more (1000 unless given); the second starts from that state and runs the
# same. Both have to end as the same machine: its memory, its registers and its clock. fdc is 1
# for a script whose discs need the A5000's floppy controller.
#
#   bash tests/savestate.sh ~/pab/t/lem.script
HERE=$(realpath "$(dirname "$0")/..")
CS=${2:-1000}
T=$(mktemp -d /tmp/pabs-XXXXXX)
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy

{ cat "$1"; printf 'savestate %s/state\nwait %s\n' "$T" "$CS"; } > "$T/first"
printf 'loadstate %s/state\nwait %s\n' "$T" "$CS" > "$T/second"
a=$("$HERE/build/piarcboot" test "$HERE/ROM311" "$T/first" ${3:-0})
b=$("$HERE/build/piarcboot" test "$HERE/ROM311" "$T/second" ${3:-0})
size=$(stat -c %s "$T/state" 2>/dev/null)
rm -rf "$T"
echo "taken:    $a"
echo "restored: $b"
echo "state file: ${size:-missing} bytes"
[ -n "$a" ] && [ "$a" = "$b" ] && echo "same" && exit 0
echo "DIFFERENT"
exit 1
