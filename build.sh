#!/usr/bin/env bash
# Builds Stremio for PS5 and packages it. Run inside WSL/Ubuntu:
#
#   ./build.sh            install the toolchain if needed, build, package
#   ./build.sh desktop    build a Linux version for testing on the PC
#
# Output (dist/):
#   PPSA77711.ffpfsc      the whole app as one image file: copy it to
#                         /data/homebrew/ (ShadowMountPlus mounts it). File
#                         permissions are inside it, so they survive any copy
#   PPSA77711/            the same as a folder, for /data/homebrew/PPSA77711
#                         (needs its execute permission restored after copying
#                         with tools that drop it, like PS5 Upload)
#   PPSA77711.zip         the folder, zipped
#
# Stremio is the title's own eboot.bin: one native app, no launcher, no ELF
# loader. See native/build.sh for how it is linked.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APP_FILES="${APP_FILES:-$HERE/app}"   # assets, fonts, ca-bundle.crt, sce_sys
SDK="${PS5_PAYLOAD_SDK:-/opt/ps5-payload-sdk}"
BUILD="${BUILD_DIR:-$HOME/stremio-build}"
JOBS="$(nproc)"

need_apt() {
    local missing=()
    for p in "$@"; do dpkg -s "$p" >/dev/null 2>&1 || missing+=("$p"); done
    if [ ${#missing[@]} -gt 0 ]; then
        echo ">> installing ${missing[*]}"
        sudo apt-get update
        sudo apt-get install -y "${missing[@]}"
    fi
}

if [ "${1:-}" = "desktop" ]; then
    need_apt build-essential cmake pkg-config git libsdl2-dev libfreetype-dev libcurl4-openssl-dev \
             libavformat-dev libavcodec-dev libavutil-dev libswscale-dev libswresample-dev libwebp-dev
    if ! [ -e /usr/local/lib/cmake/RmlUi/RmlUiConfig.cmake ]; then
        echo ">> building RmlUi 6.2"
        rm -rf /tmp/RmlUi && git clone --depth 1 --branch 6.2 https://github.com/mikke89/RmlUi.git /tmp/RmlUi
        cmake -S /tmp/RmlUi -B /tmp/RmlUi/build -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON
        make -C /tmp/RmlUi/build -j"$JOBS"
        sudo make -C /tmp/RmlUi/build install
        sudo ldconfig
    fi
    cmake -S "$HERE" -B "$BUILD/desktop" -DCMAKE_BUILD_TYPE=Release
    make -C "$BUILD/desktop" -j"$JOBS"
    echo
    echo "Built $BUILD/desktop/stremio"
    echo "Run:  STREMIO_BASE=$APP_FILES $BUILD/desktop/stremio"
    exit 0
fi

# ---------------------------------------------------------------------------
# Toolchain: the PS5 payload SDK with prebuilt SDL2, RmlUi, FFmpeg, curl, ...
need_apt clang-18 lld-18 llvm-18 cmake make pkg-config python3 wget zip git ninja-build ccache python3-venv unzip curl

if ! [ -e "$SDK/target/user/homebrew/lib/librmlui.a" ]; then
    echo ">> downloading the PS5 payload SDK + libraries (about 330 MB)"
    wget -q --show-progress -O /tmp/ps5-payload-dev.tar.gz \
        https://github.com/ps5-payload-dev/pacbrew-repo/releases/latest/download/ps5-payload-dev.tar.gz
    sudo tar xf /tmp/ps5-payload-dev.tar.gz -C /
    rm -f /tmp/ps5-payload-dev.tar.gz
fi
export PS5_PAYLOAD_SDK="$SDK"

# The native app's PS5 tooling (converter, signer, libc.prx loader shim) comes
# from ps5-native-app-boilerplate.
BP="${BOILERPLATE_DIR:-$HOME/ps5-native-app-boilerplate}"
BP_COMMIT=f98de73
if ! [ -d "$BP/.git" ]; then
    git clone -q https://github.com/blackbearreloaded/ps5-native-app-boilerplate.git "$BP"
fi
git -C "$BP" fetch -q origin && git -C "$BP" checkout -q "$BP_COMMIT"
make -C "$BP" deps >/dev/null
[ -f "$BP/runtime/libc.prx" ] || (cd "$BP" && bash tools/rebuild-libc.sh >/dev/null)
(cd "$BP" && bash tools/build-host-tools.sh >/dev/null)

# ---------------------------------------------------------------------------
rm -rf "$HERE/dist"
BOILERPLATE_DIR="$BP" APP_FILES="$APP_FILES" bash "$HERE/native/build.sh" PPSA77711 "Stremio"
# One image file with the permissions inside (copy tools can't strip them).
BOILERPLATE_DIR="$BP" bash "$HERE/native/pack.sh" PPSA77711 | tail -1
echo
echo "Done: copy dist/PPSA77711.ffpfsc to /data/homebrew/"
echo "      (or the folder dist/PPSA77711 to /data/homebrew/PPSA77711)"
ls -la "$HERE/dist" "$HERE/dist/PPSA77711/sce_sys"
