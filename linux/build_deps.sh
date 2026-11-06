#!/bin/bash
# aarch64 libraries for linux/CMakeLists.txt, installed into linux/aarch64.
# SDL3 3.4.16, Vulkan-Headers and Vulkan-Loader 1.4.350.1, VMA 3.3.0, shaderc 2026.3.
# On x86-64 this cross-compiles with linux/toolchain-aarch64.cmake. Each piece is skipped when its output exists.
set -euo pipefail
cd "$(dirname "$0")"
D=$PWD
S=$D/src
P=$D/aarch64
W=$D/work
mkdir -p "$S" "$P/include" "$P/lib" "$W"
JOBS=$(nproc 2>/dev/null || echo 8)
command -v git >/dev/null || { apt-get update -qq && apt-get install -y -qq git ca-certificates; }
if [ "$(uname -m)" = "aarch64" ]; then TC=(); else TC=(-DCMAKE_TOOLCHAIN_FILE="$D/toolchain-aarch64.cmake"); fi

fetch() {
    if [ ! -f "$S/$3" ]; then
        curl -fsSL -o "$S/$3.part" "$1" && mv "$S/$3.part" "$S/$3"
    fi
    (cd "$S" && echo "$2  $3" | sha256sum -c --quiet)
}
unpack() { rm -rf "$W/$2" && mkdir -p "$W/$2" && tar -xf "$S/$1" -C "$W/$2" --strip-components 1; }

fetch https://github.com/libsdl-org/SDL/releases/download/release-3.4.16/SDL3-3.4.16.tar.gz \
      7322236cd12090c3eb40b9728be4d49c76f66ad17d04369584d4ecad5cf77c68 SDL3-3.4.16.tar.gz
fetch https://github.com/google/shaderc/archive/refs/tags/v2026.3.tar.gz \
      ee493ccf1b3038b4ef2fe024664c5eb2dc4bcc1f6b05b33e3909de0e19c81024 shaderc-2026.3.tar.gz
fetch https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/vulkan-sdk-1.4.350.1.tar.gz \
      6d1bb65e49520344cc0a48af3dc02e993781efff14c7ebdcb8ae9fa23ddf7e83 Vulkan-Headers-1.4.350.1.tar.gz
fetch https://github.com/KhronosGroup/Vulkan-Loader/archive/refs/tags/vulkan-sdk-1.4.350.1.tar.gz \
      602984a71000981e25e4feb419e6cdd70b18ffe2b8004f60f591706027bca468 Vulkan-Loader-1.4.350.1.tar.gz
fetch https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/archive/refs/tags/v3.3.0.tar.gz \
      c4f6bbe6b5a45c2eb610ca9d231158e313086d5b1a40c9922cb42b597419b14e VulkanMemoryAllocator-3.3.0.tar.gz

if [ ! -f "$P/include/vulkan/vulkan_core.h" ]; then
    unpack Vulkan-Headers-1.4.350.1.tar.gz vkh
    cmake -S "$W/vkh" -B "$W/vkh/build" "${TC[@]}" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$P"
    cmake --install "$W/vkh/build"
fi
if [ ! -f "$P/include/vma/vk_mem_alloc.h" ]; then
    unpack VulkanMemoryAllocator-3.3.0.tar.gz vma
    mkdir -p "$P/include/vma"
    cp "$W/vma/include/vk_mem_alloc.h" "$P/include/vma/"
fi
if [ ! -e "$P/lib/libvulkan.so" ]; then
    unpack Vulkan-Loader-1.4.350.1.tar.gz vkl
    cmake -S "$W/vkl" -B "$W/vkl/build" "${TC[@]}" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$P" \
        -DUPDATE_DEPS=OFF -DBUILD_TESTS=OFF -DBUILD_WSI_XCB_SUPPORT=OFF -DBUILD_WSI_XLIB_SUPPORT=OFF \
        -DBUILD_WSI_WAYLAND_SUPPORT=OFF -DBUILD_WSI_DIRECTFB_SUPPORT=OFF \
        -DVULKAN_HEADERS_INSTALL_DIR="$P"
    cmake --build "$W/vkl/build" -j"$JOBS"
    cmake --install "$W/vkl/build"
fi
# aarch64 X11 libraries (headers + .so). Ubuntu 24.04 has no arm64 multiarch here, so the debs are extracted.
X11_PKGS="libx11-6 libx11-dev libx11-xcb1 libxau6 libxcb1 libxcursor1 libxcursor-dev libxdmcp6 libxext6 libxext-dev libxfixes3 libxfixes-dev libxi6 libxi-dev libxrandr2 libxrandr-dev libxrender1 libxrender-dev libxss1 libxss-dev"
if [ ! -f "$P/lib/libX11.so" ]; then
    python3 - "$S" "$W/x11debs" << 'PY'
import gzip, sys, urllib.request
from pathlib import Path
src, dest = Path(sys.argv[1]), Path(sys.argv[2])
dest.mkdir(parents=True, exist_ok=True)
want = set("""libx11-6 libx11-dev libx11-xcb1 libxau6 libxcb1 libxcursor1 libxcursor-dev libxdmcp6 libxext6 libxext-dev libxfixes3 libxfixes-dev libxi6 libxi-dev libxrandr2 libxrandr-dev libxrender1 libxrender-dev libxss1 libxss-dev""".split())
url = "http://ports.ubuntu.com/ubuntu-ports/dists/noble/main/binary-arm64/Packages.gz"
text = gzip.decompress(urllib.request.urlopen(url, timeout=120).read()).decode()
cur = fn = None
for line in text.splitlines():
    if line.startswith("Package: "):
        cur = line.split()[1]
    elif line.startswith("Filename: ") and cur in want:
        fn = line.split()[1]
        out = dest / Path(fn).name
        if not out.exists():
            urllib.request.urlretrieve("http://ports.ubuntu.com/ubuntu-ports/" + fn, out)
        print(out.name)
PY
    rm -rf "$W/x11root" && mkdir -p "$W/x11root"
    for deb in "$W"/x11debs/*.deb; do dpkg-deb -x "$deb" "$W/x11root"; done
    mkdir -p "$P/include" "$P/lib/pkgconfig"
    cp -a "$W/x11root/usr/include/." "$P/include/"
    cp -a "$W/x11root/usr/lib/aarch64-linux-gnu/." "$P/lib/"
    # pc files say prefix=/usr and libdir under aarch64-linux-gnu; point them at this prefix
    if [ -d "$P/lib/pkgconfig" ]; then
        sed -i "s|^prefix=/usr|prefix=$P|; s|lib/aarch64-linux-gnu|lib|g" "$P/lib/pkgconfig/"*.pc
    fi
fi
if [ ! -f "$P/include/xcb/xcb.h" ]; then
    mkdir -p "$W/x11debs"
    python3 - "$W/x11debs" << 'PY'
import gzip, sys, urllib.request
from pathlib import Path
dest = Path(sys.argv[1])
want = {"libxcb1-dev", "libxau-dev", "libxdmcp-dev"}
url = "http://ports.ubuntu.com/ubuntu-ports/dists/noble/main/binary-arm64/Packages.gz"
text = gzip.decompress(urllib.request.urlopen(url, timeout=120).read()).decode()
cur = None
for line in text.splitlines():
    if line.startswith("Package: "):
        cur = line.split()[1]
    elif line.startswith("Filename: ") and cur in want:
        fn = line.split()[1]
        out = dest / Path(fn).name
        if not out.exists():
            urllib.request.urlretrieve("http://ports.ubuntu.com/ubuntu-ports/" + fn, out)
        print(out.name)
PY
    rm -rf "$W/xcbroot" && mkdir -p "$W/xcbroot"
    for deb in "$W"/x11debs/libxcb1-dev_*.deb "$W"/x11debs/libxau-dev_*.deb "$W"/x11debs/libxdmcp-dev_*.deb; do
        dpkg-deb -x "$deb" "$W/xcbroot"
    done
    cp -a "$W/xcbroot/usr/include/." "$P/include/"
    cp -a "$W/xcbroot/usr/lib/aarch64-linux-gnu/." "$P/lib/"
fi
if [ ! -f "$P/include/X11/X.h" ]; then
    curl -fsSL -o "$W/x11proto-dev.deb" http://archive.ubuntu.com/ubuntu/pool/main/x/xorgproto/x11proto-dev_2023.2-1_all.deb
    rm -rf "$W/xproto" && mkdir -p "$W/xproto"
    dpkg-deb -x "$W/x11proto-dev.deb" "$W/xproto"
    mkdir -p "$P/include"
    cp -a "$W/xproto/usr/include/." "$P/include/"
fi
if [ ! -f "$P/lib/libSDL3.so" ]; then
    unpack SDL3-3.4.16.tar.gz sdl
    PKG_CONFIG_PATH="$P/lib/pkgconfig" PKG_CONFIG_SYSROOT_DIR= \
    cmake -S "$W/sdl" -B "$W/sdl/build" "${TC[@]}" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$P" \
        -DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_INCLUDE_PATH="$P/include" -DCMAKE_LIBRARY_PATH="$P/lib" \
        -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF \
        -DSDL_X11=ON -DSDL_X11_XTEST=OFF -DSDL_X11_XDBE=OFF -DSDL_X11_XSHAPE=OFF -DSDL_X11_XSYNC=OFF \
        -DSDL_WAYLAND=OFF -DSDL_KMSDRM=OFF -DSDL_OPENGL=OFF -DSDL_OPENGLES=OFF -DSDL_VULKAN=ON \
        -DSDL_ALSA=OFF -DSDL_PULSEAUDIO=OFF -DSDL_PIPEWIRE=OFF -DSDL_JACK=OFF -DSDL_SNDIO=OFF \
        -DSDL_LIBUDEV=OFF -DSDL_DBUS=OFF -DSDL_IBUS=OFF
    cmake --build "$W/sdl/build" -j"$JOBS"
    cmake --install "$W/sdl/build"
fi
if [ ! -f "$P/lib/libshaderc_shared.so" ]; then
    unpack shaderc-2026.3.tar.gz shaderc
    (cd "$W/shaderc" && python3 utils/git-sync-deps)
    cmake -S "$W/shaderc" -B "$W/shaderc/build" "${TC[@]}" -DCMAKE_BUILD_TYPE=Release \
        -DSHADERC_SKIP_TESTS=ON -DSHADERC_SKIP_EXAMPLES=ON -DSHADERC_SKIP_COPYRIGHT_CHECK=ON \
        -DSPIRV_SKIP_TESTS=ON -DSPIRV_SKIP_EXECUTABLES=ON -DENABLE_GLSLANG_BINARIES=OFF \
        -DSPIRV_TOOLS_BUILD_STATIC=ON -DBUILD_SHARED_LIBS=OFF
    cmake --build "$W/shaderc/build" --target shaderc_shared -j"$JOBS"
    mkdir -p "$P/include/shaderc"
    cp "$W/shaderc/libshaderc/include/shaderc/"*.h "$P/include/shaderc/"
    cp "$W/shaderc/build/libshaderc/libshaderc_shared.so" "$P/lib/"
fi
echo "deps: $P"
ls -l "$P/lib"
