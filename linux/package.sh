#!/bin/bash
# One archive for an aarch64 Linux handheld.
# The handheld supplies libvulkan.so.1 (its GPU driver), FFmpeg, glibc, libstdc++, and X11/Xwayland.
# Usage: linux/package.sh --no-game
#        linux/package.sh <path-to-botw-1.9.0>     (also packs that copy's exefs and romfs)
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD=$ROOT/build/aarch64
DEPS=$ROOT/linux/aarch64/lib

usage() {
    echo "usage: linux/package.sh --no-game" >&2
    echo "       linux/package.sh <path-to-botw-1.9.0>" >&2
    exit 1
}

NO_GAME=0
GAME=
if [ "${1:-}" = "--no-game" ]; then
    NO_GAME=1
    shift
    [ $# -eq 0 ] || usage
elif [ $# -eq 1 ]; then
    GAME=$1
else
    usage
fi
NAME=BotwRecomp-linux-aarch64
OUT=$ROOT/dist
STAGE=$(mktemp -d)
DEST=$STAGE/$NAME
trap 'rm -rf "$STAGE"' EXIT

test -f "$BUILD/botwrecomp"
test -f "$BUILD/botwgame.so"
test -f "$BUILD/botwgame.so.id"
test -f "$DEPS/libSDL3.so.0.4.16"
test -f "$DEPS/libshaderc_shared.so"
if [ "$NO_GAME" = 0 ]; then
    test -f "$GAME/exefs/main"
    test -f "$GAME/romfs/System/Version.txt"
fi
test -f "$ROOT/third_party/fonts/Baloo2/Baloo2-SemiBold.ttf"
test -f "$ROOT/third_party/fonts/DejaVu/DejaVuSans.ttf"
command -v patchelf >/dev/null

mkdir -p "$DEST/lib" "$DEST/extracted" "$OUT"
cp "$BUILD/botwrecomp" "$DEST/botwrecomp"
cp "$BUILD/botwgame.so" "$DEST/botwgame.so"
cp "$BUILD/botwgame.so.id" "$DEST/botwgame.so.id"
printf '190\n' > "$DEST/game_version.txt"
if [ "$NO_GAME" = 0 ]; then
    mkdir -p "$DEST/v190/extracted/exefs"
    cp "$BUILD/botwgame.so" "$DEST/v190/extracted/botwgame.so"
    cp "$BUILD/botwgame.so.id" "$DEST/v190/extracted/botwgame.so.id"
    cp -a "$GAME/exefs/." "$DEST/v190/extracted/exefs/"
    printf 'extracted/romfs\n' > "$DEST/v190/extracted/romfs.link"
fi
cp "$DEPS/libSDL3.so.0.4.16" "$DEST/lib/libSDL3.so.0.4.16"
ln -s libSDL3.so.0.4.16 "$DEST/lib/libSDL3.so.0"
cp "$DEPS/libshaderc_shared.so" "$DEST/lib/libshaderc_shared.so.1"
# The start screen looks for Baloo beside the executable. Without it ImGui's 13px default font is used.
# DejaVu Sans is the in-game system font when the player has not imported a Switch dump (runtime/hle/pl.c).
cp "$ROOT/third_party/fonts/Baloo2/Baloo2-SemiBold.ttf" "$DEST/Baloo2-SemiBold.ttf"
cp "$ROOT/third_party/fonts/Baloo2/OFL.txt" "$DEST/Baloo2-OFL.txt"
cp "$ROOT/third_party/fonts/DejaVu/DejaVuSans.ttf" "$DEST/DejaVuSans.ttf"
cp "$ROOT/third_party/fonts/DejaVu/LICENSE" "$DEST/DejaVu-LICENSE.txt"
chmod 755 "$DEST/botwrecomp" "$DEST/lib/libSDL3.so.0.4.16" "$DEST/lib/libshaderc_shared.so.1"

# The build tree keeps its absolute run path. Only this copy looks beside itself.
patchelf --set-rpath '$ORIGIN/lib' "$DEST/botwrecomp"

if [ "$NO_GAME" = 1 ]; then
cat > "$DEST/README.txt" << 'EOF'
BOTW recomp for Linux aarch64.

No game files are in this archive. You need your own legally obtained, extracted
files (a folder with exefs/ and romfs/) for version 1.0.0, 1.6.0, 1.8.2, or 1.9.0.
The start screen asks for that folder and sets it up. This archive does not say
where to get those files.

Unpack the archive, cd into BotwRecomp-linux-aarch64, and run ./botwrecomp
from that folder. The empty extracted/ folder makes that folder the data
directory. game_version.txt starts on 1.9.0. botwgame.so next to the executable
is the recompiled game code, not a copy of the console's files.
Baloo2-SemiBold.ttf next to the executable is the start screen's font.
DejaVuSans.ttf next to it is the in-game text substitute until a Switch font
dump is imported. lib/ holds SDL3 and shaderc. The handheld's own
libvulkan.so.1 is used, so its GPU driver loads, and its own FFmpeg is used
for movies.

Needs glibc 2.38 or newer, Vulkan 1.3, FFmpeg (libavformat.so.62,
libavcodec.so.62, libavutil.so.60, libswscale.so.9), and X11 or Xwayland
(Gamescope's Xwayland is enough). Page size 4K or 16K. This binary does not
run on x86-64 (a Steam Deck).
EOF
else
cat > "$DEST/README.txt" << 'EOF'
BOTW recomp for Linux aarch64, with this copy's 1.9.0 game files.

Unpack the archive, cd into BotwRecomp-linux-aarch64, and run ./botwrecomp
from that folder. The empty extracted/ folder makes that folder the data
directory. game_version.txt selects 1.9.0, whose files are in v190/extracted
(exefs, romfs, and botwgame.so). Baloo2-SemiBold.ttf next to the executable is the
start screen's font. DejaVuSans.ttf next to it is the in-game text substitute until a
Switch font dump is imported. lib/ holds SDL3 and shaderc. The handheld's
own libvulkan.so.1 is used, so its GPU driver loads, and its own FFmpeg is used
for movies.

Needs glibc 2.38 or newer, Vulkan 1.3, FFmpeg (libavformat.so.62,
libavcodec.so.62, libavutil.so.60, libswscale.so.9), and X11 or Xwayland
(Gamescope's Xwayland is enough). Page size 4K or 16K. This binary does not
run on x86-64 (a Steam Deck).
EOF
fi

if [ "$NO_GAME" = 1 ]; then
    echo "packing runtime only (no exefs, no romfs)"
    tar -C "$STAGE" -cf - "$NAME" | gzip -1 > "$OUT/$NAME.tar.gz"
else
    echo "packing $GAME (romfs is about 15 GB; gzip is stored almost as-is)"
    tar -C "$STAGE" -cf - "$NAME" \
        -C "$GAME" --transform 's,^romfs,BotwRecomp-linux-aarch64/v190/extracted/romfs,' \
        romfs | gzip -1 > "$OUT/$NAME.tar.gz"
fi
echo "wrote $OUT/$NAME.tar.gz"
ls -lh "$OUT/$NAME.tar.gz"
aarch64-linux-gnu-readelf -d "$DEST/botwrecomp" | grep RUNPATH
