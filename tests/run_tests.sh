#!/usr/bin/env bash
# Builds the AVI writer for the host and validates its output with ffmpeg.
set -euo pipefail
cd "$(dirname "$0")"
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT

g++ -std=gnu++11 -Wall -Wextra -Werror -I../plugin/Includes \
    avi_writer_test.cpp ../plugin/Sources/AviWriter.cpp -o "$out/avitest"

for mode in 1 0; do
    "$out/avitest" "$out/clip$mode.avi" "$mode" >/dev/null
    info=$(ffprobe -v error -select_streams v:0 -count_frames \
        -show_entries stream=codec_name,width,height,r_frame_rate,nb_read_frames,nb_frames \
        -of default=nw=1 "$out/clip$mode.avi")
    echo "--- finished=$mode"; echo "$info"
    grep -q "codec_name=rawvideo" <<<"$info"
    grep -q "width=400" <<<"$info"
    grep -q "height=240" <<<"$info"
    grep -q "r_frame_rate=10/1" <<<"$info"

    # Decoding must not report any errors
    ffmpeg -v error -xerror -i "$out/clip$mode.avi" -f null - 

    # 100 real + 30 dropped frames = 13 seconds at 10 fps. A recording that was
    # cut off only knows the length from the last periodic header update.
    dur=$(ffprobe -v error -show_entries format=duration -of csv=p=0 "$out/clip$mode.avi")
    echo "duration=$dur"
    if [[ $mode == 1 ]]; then [[ "$dur" == 13.0* ]]; else [[ "$dur" == 12.0* ]]; fi

    # Top-left pixel of frame 0 must be blue, bottom-left row green (bottom-up rows)
    rgb=$(ffmpeg -v error -i "$out/clip$mode.avi" -frames:v 1 -vf "crop=1:1:0:0" -f rawvideo -pix_fmt rgb24 - | od -An -tu1 | tr -s ' ')
    echo "top-left rgb:$rgb"; [[ "$rgb" == " 0 0 255"* || "$rgb" == " 0 0 24"* ]]
    rgb=$(ffmpeg -v error -i "$out/clip$mode.avi" -frames:v 1 -vf "crop=1:1:0:239" -f rawvideo -pix_fmt rgb24 - | od -An -tu1 | tr -s ' ')
    echo "bottom-left rgb:$rgb"; [[ "$rgb" == " 0 255 0"* || "$rgb" == " 0 24"* ]]
done
echo "ALL TESTS PASSED"
