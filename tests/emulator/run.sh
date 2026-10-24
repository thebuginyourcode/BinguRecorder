#!/usr/bin/env bash
# Records 20 seconds of the test app with BinguRecorder in the Azahar
# emulator and checks the result. See README.md for building the emulator.
#
# usage: AZAHAR_CORE=/path/to/azahar_libretro.so tests/emulator/run.sh [work dir]
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
: "${AZAHAR_CORE:?Set AZAHAR_CORE to the azahar_libretro.so built as described in README.md}"
: "${DEVKITARM:?Set DEVKITARM (devkitARM with libctru and makerom in PATH)}"
work=${1:-$(mktemp -d)}

make -C "$here/testapp" >/dev/null
make -C "$repo/plugin" >/dev/null
g++ -std=c++17 -O2 -I"$(dirname "$AZAHAR_CORE")/../../../externals/libretro-common/libretro-common/include" \
    "$here/host/retro_host.cpp" -ldl -o "$work/retro_host" 2>/dev/null \
    || g++ -std=c++17 -O2 -I"${LIBRETRO_INCLUDE:?Set LIBRETRO_INCLUDE to the folder with libretro.h}" \
       "$here/host/retro_host.cpp" -ldl -o "$work/retro_host"

sd="$work/Azahar/sdmc"
# Keep settings.bin (lets a test pick settings), drop old recordings
rm -f "$sd"/BinguRecorder/*.avi "$sd"/BinguRecorder/*.txt
mkdir -p "$sd/luma/plugins" "$sd/3ds"
# The test app is homebrew (libctru), so use a copy allowed to load into it
python3 "$here/allow_homebrew.py" "$repo/plugin/BinguRecorder.3gx" "$sd/luma/plugins/default.3gx" >/dev/null
# libctru needs the sound firmware file to exist; the emulator ignores its contents
[[ -f "$sd/3ds/dspfirm.cdc" ]] || head -c 8192 /dev/zero > "$sd/3ds/dspfirm.cdc"

# L+R+Down starts recording at 10 s and stops it at 30 s
"$work/retro_host" "$AZAHAR_CORE" "$here/testapp/BinguTest.cxi" "$work" 2600 \
    --press 600:610:l+r+down --press 1800:1810:l+r+down > "$work/emulator.log" 2>&1

if grep -q "Plugin error" "$work/emulator.log"; then
    grep "Plugin error" "$work/emulator.log"
    exit 1
fi

avi=$(ls "$sd"/BinguRecorder/*.avi)
echo "recording: $avi"
cat "${avi%.avi}.txt"
echo
python3 "$here/analyze.py" "$avi"
