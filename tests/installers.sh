#!/bin/bash
# installers.sh <out folder> [game ...]: the JASPP games that came with a hard disc installer, all
# of those with a script in tests/installers/ unless some are named.
#
# Each is installed into a scratch store, which downloads it and copies the files that its
# presets/<game>.install lists from its discs onto its HostFS folder. Then its installer itself is
# run, worked as a person would by tests/installers/<game>.script on a machine with no window,
# against copies of its discs and onto an empty HostFS folder, <out>/<game>/hostfs, and the two
# must hold the same files. Last the game starts from its HostFS folder, removed first so that it
# is put back from installed/, and <out>/<game>.png is its screen 40 seconds in.
#
# A game with a script and no copy list yet installs to play from its floppies, and what its
# installer copies is left in <out>/<game>/hostfs to write the list from, with piarcboot ls on
# each of its discs to show where each file came from.
#
# The scripts are piarcboot test scripts, with "disc <drive> <n>" for the game's disc n. Besides
# wait, key, type and run they use pointer <x> <y> (OS units from the bottom left), button
# l|m|r down|up, untilerror <cs> (until an error box opens, as for a disc to put in) and
# untilidle <cs> <most> (until the floppy drive has been still that long, as it is once an
# installer has finished). ADFS only looks for another disc when it starts the drive again, so a
# script waits for the drive to stop before it changes the disc.
OUT=$(realpath -m "$1")
shift
HERE=$(realpath "$(dirname "$0")/..")
export PIARCBOOT_HOME=$(mktemp -d /tmp/pabi-XXXXXX)
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
GAMES=${*:-$(cd "$HERE/tests/installers" && ls *.script | sed 's/\.script$//')}
mkdir -p "$OUT"

for g in $GAMES; do
	"$HERE/build/piarcboot" install "$g" > "$OUT/$g.log" 2>&1 &
done
wait
EXTROM=$(ls -d "$PIARCBOOT_HOME"/games/*/extrom 2>/dev/null | head -1)

# each installer run against copies of the game's discs, as it may write to them
for g in $GAMES; do
	d=$PIARCBOOT_HOME/games/$g
	w=$OUT/$g
	rm -rf "$w"
	mkdir -p "$w/hostfs" "$w/discs"
	discs=$(grep '^disc=' "$d/game.ini" | cut -d= -f2)
	for k in $discs; do cp "$d/$k" "$w/discs/"; done
	awk -v dir="$w/discs" -v list="$discs" '
		BEGIN { split(list, D, " "); print "disc 0 " dir "/" D[1] }
		$1 == "disc" && $3 ~ /^[0-9]+$/ { print "disc " $2 " " dir "/" D[$3]; next }
		{ print }' "$HERE/tests/installers/$g.script" > "$w/script"
	PIARCBOOT_EXTROM=${EXTROM:-$d/extrom} PIARCBOOT_HOSTFS=$w/hostfs \
		"$HERE/build/piarcboot" test "$HERE/ROM311" "$w/script" >> "$OUT/$g.log" 2>&1 &
done
wait

fails=0
for g in $GAMES; do
	d=$PIARCBOOT_HOME/games/$g
	n=$(find "$OUT/$g/hostfs" -type f | wc -l)
	if [ ! -d "$d/installed" ]; then
		echo "$g: no copy list; its installer copied $n files, in $OUT/$g/hostfs"
		continue
	fi
	same=$(diff -r "$d/installed" "$OUT/$g/hostfs" > "$OUT/$g.diff" && echo yes || echo no)
	rm -rf "$d/hostfs"
	printf '4000 shot %s\n4100 quit\n' "$OUT/$g.png" > "$OUT/$g.test"
	timeout 300 "$HERE/build/piarcboot" play "$g" --test "$OUT/$g.test" >> "$OUT/$g.log" 2>&1
	back=$(diff -r "$d/installed" "$d/hostfs" > /dev/null 2>&1 && echo yes || echo no)
	ok=$([ "$same" = yes ] && [ "$back" = yes ] && [ -s "$OUT/$g.png" ] && echo ok || echo FAIL)
	[ "$ok" = ok ] || fails=$((fails + 1))
	echo "$g: $n files, the same as its installer's: $same, put back: $back, $ok"
done
rm -rf "$PIARCBOOT_HOME"
exit $fails
