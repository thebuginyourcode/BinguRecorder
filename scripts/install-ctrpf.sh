#!/usr/bin/env bash
# Builds and installs CTRPluginFramework and 3gxtool (needed to build 3GX
# plugins) into $DEVKITPRO. Run inside a devkitARM environment, for example
# the devkitpro/devkitarm Docker image.
set -euo pipefail

: "${DEVKITPRO:?Set DEVKITPRO (usually /opt/devkitpro)}"
CTRPF_REV=6671936663faaa900f7cfdeedf42bc8581560f4f
GXTOOL_REV=57e3160ab72d8533a2f0601efdc8a149b4825ea5

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

fetch() { # url rev dir
    git init -q "$3"
    git -C "$3" fetch -q --depth 1 "$1" "$2"
    git -C "$3" checkout -q FETCH_HEAD
}

fetch https://gitlab.com/thepixellizeross/3gxtool.git "$GXTOOL_REV" "$work/3gxtool"
git -C "$work/3gxtool" submodule update -q --init --depth 1
cmake -S "$work/3gxtool" -B "$work/3gxtool/build" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$work/3gxtool/build" -j"$(nproc)" >/dev/null
install -m755 "$work/3gxtool/build/3gxtool" "$DEVKITPRO/tools/bin/3gxtool"

fetch https://gitlab.com/thepixellizeross/ctrpluginframework.git "$CTRPF_REV" "$work/ctrpf"
# Newer compilers warn about code in the framework; don't fail on that.
sed -i 's/-Wall -Werror/-Wall/' "$work/ctrpf/Library/Makefile"
# Bug fix: the svcMapMemoryBlock hook search restarts after each match but
# keeps searching the full code size, reading past the end of the game's
# code (a crash when several svc 0x1F instructions come first).
python3 - "$work/ctrpf/Library/source/pluginInit.cpp" <<'PY'
import sys
path = sys.argv[1]
src = open(path).read()
old = "Utils::Search<u32>(svcMapMemoryBlockAddr + 4, Process::GetTextSize(), {0xEF00001F})"
# Remaining whole words up to the end of the code (never negative, and a
# multiple of 4: Search() misbehaves when the size is smaller than the pattern)
new = ("Utils::Search<u32>(svcMapMemoryBlockAddr + 4, [&]() -> u32 { "
       "const u32 s = svcMapMemoryBlockAddr + 4, e = (0x00100000 + Process::GetTextSize()) & ~3u; "
       "return e > s ? e - s : 0u; }(), {0xEF00001F})")
assert src.count(old) == 1, "CTRPF changed, check the svcMapMemoryBlock search patch"
open(path, 'w').write(src.replace(old, new))
PY
# The release and debug libraries share a temporary file, so building them
# at the same time (make -j) can fail. Build them one after the other.
make -C "$work/ctrpf/Library" -j"$(nproc)" lib/libctrpf.a >/dev/null
make -C "$work/ctrpf/Library" -j"$(nproc)" lib/libctrpfd.a >/dev/null
make -C "$work/ctrpf/Library" install >/dev/null
# The install step only ships part of the headers the public ones include.
cp -r "$work/ctrpf/Library/include/." "$DEVKITPRO/libctrpf/include/"

echo "CTRPluginFramework and 3gxtool installed in $DEVKITPRO"
