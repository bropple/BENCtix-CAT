#!/bin/sh
# Runs on the phone as root, from the directory holding t9-replay, the key
# scripts and the screen scripts. Plays a demo on tty7 through a second
# s22-t9d fed by a virtual keypad (the real keypad's s22-t9d keeps running)
# and records the top ROWS text rows of fb0 (default all 40: the whole
# 480x640 portrait screen) into CAP/cap.raw.
#   message demo: CAP=/home/t9demo ./run-demo.sh
#   shell demo:   KEYS=shell.keys SCREEN=shell-screen.sh \
#                 T9ARGS="-t 450 -l 350" CAP=/home/t9demo ./run-demo.sh
KEYS=${KEYS:-demo.keys}
SCREEN=${SCREEN:-demo-screen.sh}
ROWS=${ROWS:-40}
T9ARGS=${T9ARGS:-}
# Another s22-t9d build to type with (e.g. one built with -v support)
T9BIN=${T9BIN:-/usr/bin/s22-t9d}
cd "$(dirname "$0")"
here=$PWD
CAP=${CAP:-$here}
mkdir -p "$CAP"
back=$(fgconsole)

openvt -c 7 -f -- sh "$here/$SCREEN"
chvt 7
sleep 1
./t9-replay "$KEYS" 2500 > dev.txt &
rep=$!
sleep 0.5
"$T9BIN" $T9ARGS -d "$(head -n1 dev.txt)" -o "$here/run" 2>t9d.log &
t9=$!

: > "$CAP/cap.raw"
start=$(date +%s.%N)
frames=0
while kill -0 $rep 2>/dev/null; do
	head -c $((1920 * 16 * ROWS)) /dev/fb0 >> "$CAP/cap.raw"
	frames=$((frames + 1))
	sleep 0.07
done
end=$(date +%s.%N)
kill $t9 2>/dev/null
echo "frames=$frames start=$start end=$end rows=$ROWS" > "$CAP/cap.info"
chvt "$back"
pkill -t tty7 2>/dev/null
sleep 0.5
deallocvt 7 2>/dev/null
cat "$CAP/cap.info"
