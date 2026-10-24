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
make -C "$work/ctrpf/Library" -j"$(nproc)" install >/dev/null
# The install step only ships part of the headers the public ones include.
cp -r "$work/ctrpf/Library/include/." "$DEVKITPRO/libctrpf/include/"

echo "CTRPluginFramework and 3gxtool installed in $DEVKITPRO"
