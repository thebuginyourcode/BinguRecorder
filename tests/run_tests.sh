#!/usr/bin/env bash
# Builds the AVI writer and JPEG encoder for the host and validates their
# output with ffmpeg.
set -euo pipefail
cd "$(dirname "$0")"
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT

g++ -std=gnu++11 -O2 -Wall -Wextra -Werror -I../plugin/Includes \
    avi_writer_test.cpp ../plugin/Sources/AviWriter.cpp ../plugin/Sources/JpegEncoder.cpp \
    ../plugin/Sources/ImaAdpcm.cpp -o "$out/avitest"

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

run() { # codec width height finish [audio]
    local codec=$1 w=$2 h=$3 finish=$4 audio=${5:-none} avi="$out/clip.avi"
    echo "--- $codec ${w}x$h finished=$finish audio=$audio"
    "$out/avitest" "$avi" "$finish" "$codec" "$w" "$h" "$audio" "$out/ref.rgb" "$out/ref.pcm"

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
    if [[ $finish == 1 ]]; then [[ "$dur" == 13.0* ]]; else awk -v d="$dur" 'BEGIN { exit !(d >= 11.9 && d <= 13.0) }'; fi

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

    if [[ $audio != none ]]; then
        local channels=2 rate=32728
        [[ $audio == mono ]] && channels=1 rate=16364
        local ainfo
        ainfo=$(ffprobe -v error -select_streams a:0 \
            -show_entries stream=codec_name,sample_rate,channels -of default=nw=1 "$avi")
        echo "$ainfo" | xargs
        grep -q "codec_name=adpcm_ima_wav" <<<"$ainfo"
        grep -q "sample_rate=$rate" <<<"$ainfo"
        grep -q "channels=$channels" <<<"$ainfo"

        # Decoded audio must match the test tone and last as long as the
        # video (within one ADPCM block)
        ffmpeg -v error -xerror -i "$avi" -map 0:a -f s16le -acodec pcm_s16le -y "$out/dec.pcm"
        python3 - "$out/ref.pcm" "$out/dec.pcm" "$channels" "$rate" "$finish" <<'PY'
import math, struct, sys
ref = open(sys.argv[1], 'rb').read()
dec = open(sys.argv[2], 'rb').read()
duration = len(dec) / 2 / int(sys.argv[3]) / int(sys.argv[4])
print('audio duration=%.3f' % duration)
assert 12.9 < duration <= 13.0 or sys.argv[5] == '0', duration
n = min(len(ref), len(dec)) // 2
assert n > 100000, n
r = struct.unpack('<%dh' % n, ref[:n * 2])
d = struct.unpack('<%dh' % n, dec[:n * 2])
sig = sum(x * x for x in r)
err = sum((x - y) ** 2 for x, y in zip(r, d)) or 1
snr = 10 * math.log10(sig / err)
print('audio snr=%.1f dB' % snr)
assert snr > 25, snr
PY
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
run mjpeg 400 240 1 stereo
run mjpeg 400 240 0 stereo
run mjpeg 200 120 1 mono
run raw 400 240 1 stereo
echo "ALL TESTS PASSED"
