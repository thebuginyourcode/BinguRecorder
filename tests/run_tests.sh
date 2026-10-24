#!/usr/bin/env bash
# Builds the AVI writer and JPEG encoder for the host and validates their
# output with ffmpeg.
set -euo pipefail
cd "$(dirname "$0")"
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT

g++ -std=gnu++11 -O2 -Wall -Wextra -Werror -I../plugin/Includes \
    avi_writer_test.cpp ../plugin/Sources/AviWriter.cpp ../plugin/Sources/JpegEncoder.cpp \
    -o "$out/avitest"

pixel() { # file x y -> "r g b" of frame 0
    ffmpeg -v error -i "$1" -frames:v 1 -vf "format=rgb24,crop=1:1:$2:$3" -f rawvideo -pix_fmt rgb24 - | od -An -tu1 | xargs
}

close_to() { # "r g b" "r g b" -> each channel within 24
    local -a a=($1) b=($2)
    for i in 0 1 2; do
        local d=$(( ${a[$i]} - ${b[$i]} ))
        (( d < 0 )) && d=$(( -d ))
        (( d <= 24 )) || return 1
    done
}

run() { # codec width height finish
    local codec=$1 w=$2 h=$3 finish=$4 avi="$out/clip.avi"
    echo "--- $codec ${w}x$h finished=$finish"
    "$out/avitest" "$avi" "$finish" "$codec" "$w" "$h" "$out/ref.rgb"

    local info
    info=$(ffprobe -v error -select_streams v:0 \
        -show_entries stream=codec_name,width,height,r_frame_rate -of default=nw=1 "$avi")
    echo "$info" | xargs
    local expected=rawvideo; [[ $codec == mjpeg ]] && expected=mjpeg
    grep -q "codec_name=$expected" <<<"$info"
    grep -q "width=$w" <<<"$info"
    grep -q "height=$h" <<<"$info"
    grep -q "r_frame_rate=10/1" <<<"$info"

    # Decoding must not report any errors
    ffmpeg -v error -xerror -i "$avi" -f null -

    # 100 real + 30 dropped frames = 13 seconds at 10 fps. A recording that was
    # cut off only knows the length from the last periodic header update.
    local dur
    dur=$(ffprobe -v error -show_entries format=duration -of csv=p=0 "$avi")
    echo "duration=$dur"
    if [[ $finish == 1 ]]; then [[ "$dur" == 13.0* ]]; else [[ "$dur" == 12.0* ]]; fi

    # Orientation and colors: top-left blue, bottom-left green
    local tl bl
    tl=$(pixel "$avi" 4 4); bl=$(pixel "$avi" 4 $((h - 5)))
    echo "top-left=($tl) bottom-left=($bl)"
    close_to "$tl" "0 0 255"
    close_to "$bl" "0 255 0"

    # Picture quality of the first frame against the source
    if [[ $codec == mjpeg ]]; then
        ffmpeg -v error -i "$avi" -frames:v 1 -f rawvideo -pix_fmt rgb24 -y "$out/dec.rgb"
        local psnr
        psnr=$(ffmpeg -f rawvideo -pix_fmt rgb24 -s "${w}x$h" -i "$out/dec.rgb" \
            -f rawvideo -pix_fmt rgb24 -s "${w}x$h" -i "$out/ref.rgb" -lavfi psnr -f null - 2>&1 \
            | grep -o 'average:[0-9.]*' | cut -d: -f2)
        echo "psnr=$psnr dB"
        awk -v p="$psnr" 'BEGIN { exit !(p > 33) }'
    fi
}

run raw 400 240 1
run raw 400 240 0
run mjpeg 400 240 1
run mjpeg 400 240 0
run mjpeg 320 240 1
run mjpeg 400 480 1
run mjpeg 200 120 1     # not a multiple of 16
run mjpeg 160 120 1
echo "ALL TESTS PASSED"
