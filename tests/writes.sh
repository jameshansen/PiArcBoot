#!/bin/bash
# writes.sh <out folder>: write to blank discs and read them back. Each run starts with an image of
# zeros, formats it with ADFS's *Format, writes a 200K file of a known pattern, a small file, a
# directory and a *Copy into it, then powers on again with the same image (and the .sav beside
# it) and checks every word of both files and *Verify. F, 1.6MB, goes through the 82C711, E and
# D through it at double density, and E through the 1772. <out>/<format><fdc>.png is the last
# screen, which should read "Big errors 0", "Copy errors 0" and "Verified OK". The count of
# 82C711 commands that ended abnormally is printed: ADFS tries high density on a double density
# disc first, and each try ends with no address mark found.
OUT=$(realpath -m "$1")
HERE=$(realpath "$(dirname "$0")/..")
W=$(mktemp -d /tmp/pabw-XXXXXX)
mkdir -p "$OUT"
cat > "$W/write" <<EOF
disc 0 $W/blank.adf
wait 1500
key f12 down
wait 5
key f12 up
wait 50
type *Format 0 FORMAT WRITES Y\n
wait 15000
type *BASIC\n
wait 100
type DIM b% 204800\n
type FOR i%=0 TO 204796 STEP 4:b%!i%=i% EOR (i%<<13) EOR &5A5A5A5A:NEXT\n
wait 200
type SYS "OS_File",10,"ADFS::0.\$.Big",&FFD,,b%,b%+204800\n
wait 1000
type SYS "OS_File",10,"ADFS::0.\$.Small",&FFD,,b%,b%+1000\n
wait 300
type *CDir ADFS::0.\$.Dir\n
wait 300
type *Copy ADFS::0.\$.Small ADFS::0.\$.Dir.Copy ~C\n
wait 500
EOF
cat > "$W/read" <<EOF
disc 0 $W/blank.adf
wait 1500
key f12 down
wait 5
key f12 up
wait 50
type *Cat ADFS::0.\$\n
wait 300
type *BASIC\n
wait 100
type DIM b% 204800:E%=0\n
type SYS "OS_File",255,"ADFS::0.\$.Big",b%,0\n
wait 1000
type FOR i%=0 TO 204796 STEP 4:E%-=(b%!i%<>(i% EOR (i%<<13) EOR &5A5A5A5A)):NEXT:PRINT "Big errors ";E%\n
wait 300
type SYS "OS_File",255,"ADFS::0.\$.Dir.Copy",b%,0:E%=0\n
wait 300
type FOR i%=0 TO 996 STEP 4:E%-=(b%!i%<>(i% EOR (i%<<13) EOR &5A5A5A5A)):NEXT:PRINT "Copy errors ";E%\n
wait 300
type *Verify :0\n
wait 6000
shot SHOT
EOF
for run in "F 1638400 1" "E 819200 1" "D 819200 1" "E 819200 0"; do
  set -- $run
  rm -f "$W/blank.adf" "$W/blank.adf.sav"
  head -c "$2" /dev/zero > "$W/blank.adf"
  sed "s/FORMAT/$1/" "$W/write" > "$W/w"
  sed "s|SHOT|$OUT/$1$3.png|" "$W/read" > "$W/r"
  (cd "$HERE" && PIARCBOOT_FDC=1 ./build/piarcboot test ROM311 "$W/w" "$3" && PIARCBOOT_FDC=1 ./build/piarcboot test ROM311 "$W/r" "$3") 2> "$W/log" > /dev/null
  echo "$1 on $([ "$3" = 1 ] && echo 82C711 || echo 1772): $(grep -vc ' done' "$W/log") floppy commands, $(grep -cE '765.* done: [4-7][0-9a-f] ' "$W/log") ended abnormally, $OUT/$1$3.png"
done
rm -rf "$W"
