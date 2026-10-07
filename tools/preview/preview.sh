#!/usr/bin/env bash
# Renders the new screens to pictures (see preview_main.cpp). Run in WSL:
#
#   tools/preview/preview.sh [OUT_DIR] [screen]
#
# Needs clang-18, ninja, Mesa's software OpenGL (libegl-dev libgl-dev
# libgl1-mesa-dri) and python3 with Pillow for the sample artwork.
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
hui="$root/third_party/hui"
out=${1:-"$root/../stremio-ui-design/screens"}
build=${PREVIEW_BUILD:-/root/preview-build}
cxx=${HOST_CXX:-clang++-18}
mkdir -p "$build/obj" "$out"

art="$build/art"
[ -f "$art/poster00.png" ] && [ -f "$art/qr.png" ] || python3 "$root/tools/preview/make_sample_art.py" "$art" "$root/app/fonts/NotoSans-Bold.ttf"

sources=(
  "$root"/tools/preview/preview_main.cpp "$root"/tools/preview/sample.cpp "$root"/tools/preview/scripts_scene.cpp "$root"/tools/preview/worst_cases.cpp
  "$root"/ui/*.cpp "$root"/gl/art.cpp "$root"/gl/ime.cpp "$root"/gl/ui_sound.cpp "$root"/gl/system_status.cpp "$root"/gl/session*.cpp "$root"/src/player.cpp "$root"/src/netstream.cpp "$root"/src/hwdec_ps5.cpp "$root"/src/yuv_convert.cpp "$root"/src/subtitles.cpp "$root"/src/pcm_out.cpp "$root"/src/torrent/*.cpp "$root"/src/bidi.cpp "$root"/src/stremio.cpp "$root"/src/http.cpp "$root"/src/util.cpp "$root"/src/tasks.cpp
  "$hui"/src/gfx/*.cpp "$hui"/src/core/save_file.cpp "$hui"/src/ui/glyphs.cpp
  "$root"/tools/preview/system_host.cpp
)
objects=()
for s in "${sources[@]}"; do
  o="$build/obj/$(echo "${s#"$root/"}" | tr '/' '_').o"
  if [ ! -f "$o" ] || [ "$s" -nt "$o" ] || [ -n "$(find "$root/ui" "$hui/src" "$root/tools/preview" -name '*.hpp' -newer "$o" -print -quit)" ]; then
    "$cxx" -std=c++20 -O2 -Wall -Wextra -DGL_GLEXT_PROTOTYPES=1 \
      -I"$hui/src" -I"$root/ui" -I"$root/gl" -I"$root/src" -I"$hui/third_party/stb" -DHAVE_FRIBIDI -DHAVE_WEBP -I"$root/third_party" -I"$root/tools/preview" -Wno-missing-field-initializers -c "$s" -o "$o" &
  fi
  objects+=("$o")
done
wait
"$cxx" "${objects[@]}" -lEGL -lGL -lfribidi -lcurl -lcrypto -lwebp $(pkg-config --libs libavformat libavcodec libavutil libswresample libswscale) -lpthread -lm -o "$build/preview"
EGL_PLATFORM=surfaceless LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe \
  "$build/preview" "$hui/assets" "$art" "$root/app/assets/icons_4k" "$out" "${2:-all}"
