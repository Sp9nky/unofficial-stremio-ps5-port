#!/usr/bin/env bash
# Builds Stremio for PS5 and packages it. Run inside WSL/Ubuntu:
#
#   ./build.sh            install the toolchain if needed, build, package
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
# loader. See gl/build.sh for how it is compiled and linked.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SDK="${PS5_PAYLOAD_SDK:-/opt/ps5-payload-sdk}"

need_apt() {
    local missing=()
    for p in "$@"; do dpkg -s "$p" >/dev/null 2>&1 || missing+=("$p"); done
    if [ ${#missing[@]} -gt 0 ]; then
        echo ">> installing ${missing[*]}"
        sudo apt-get update
        sudo apt-get install -y "${missing[@]}"
    fi
}

# ---------------------------------------------------------------------------
# Toolchain: the PS5 payload SDK with prebuilt FFmpeg, curl, libwebp, ...
need_apt clang-18 lld-18 llvm-18 cmake make pkg-config python3 wget zip git ninja-build ccache python3-venv unzip curl

if ! [ -e "$SDK/target/user/homebrew/lib/libavformat.a" ]; then
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

# The OpenGL SDK (ps5-opengl, a pinned release checked against its SHA-256) is
# downloaded by ps5-homebrew-ui's own script. The toolkit's sources are in
# third_party/hui; the clone is only for that script.
KIT="${KIT_DIR:-$HOME/ps5-homebrew-ui}"
KIT_COMMIT=4bd9425
if ! [ -d "$KIT/.git" ]; then
    git clone -q https://github.com/blackbearreloaded/ps5-homebrew-ui.git "$KIT"
fi
git -C "$KIT" fetch -q origin && git -C "$KIT" checkout -q "$KIT_COMMIT"

# ---------------------------------------------------------------------------
BOILERPLATE_DIR="$BP" KIT_DIR="$KIT" bash "$HERE/gl/build.sh" PPSA77711 "Stremio"
echo
echo "Done: copy dist/PPSA77711.ffpfsc to /data/homebrew/"
echo "      (or the folder dist/PPSA77711 to /data/homebrew/PPSA77711)"
ls -la "$HERE/dist" "$HERE/dist/PPSA77711/sce_sys"
