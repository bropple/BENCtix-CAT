#!/bin/bash
# Turn run-demo.sh's capture into docs/media/NAME.{mp4,gif}.
#   encode.sh CAP.RAW FRAMES SECONDS [ROWS [NAME]]
# (FRAMES and SECONDS from cap.info: frames, end - start; ROWS default 40,
# NAME default t9-keypad-demo)
set -euo pipefail
raw=$1 frames=$2 secs=$3 rows=${4:-40} name=${5:-t9-keypad-demo}
out=$(dirname "$(readlink -f "$0")")/../../docs/media
fps=$(python3 -c "print(round($frames / $secs, 3))")
size=480x$((16 * rows))
mkdir -p "$out"
ffmpeg -loglevel error -y -f rawvideo -pixel_format bgr0 -video_size $size -framerate "$fps" -i "$raw" \
	-vf "scale=iw*2:ih*2:flags=neighbor,format=yuv420p" -c:v libx264 -crf 18 -tune animation \
	-movflags +faststart "$out/$name.mp4"
ffmpeg -loglevel error -y -f rawvideo -pixel_format bgr0 -video_size $size -framerate "$fps" -i "$raw" \
	-vf "fps=12,split[a][b];[a]palettegen=max_colors=16[p];[b][p]paletteuse=dither=none" \
	"$out/$name.gif"
ls -lh "$out"
