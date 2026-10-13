#!/usr/bin/env bash
# Offline test of the audio track a file starts with (src/audio_pick.h). Run where clang++ and FFmpeg are (the build VM):
#   bash tools/tests/run_audio_pick_test.sh
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
out=${TMPDIR:-/tmp}/audio_pick_test_bin
${CXX:-clang++-18} -std=c++20 -I"$root/src" -I"$root/third_party" "$root/tools/tests/audio_pick_test.cpp" "$root/src/stremio.cpp" \
  "$root/src/http.cpp" "$root/src/util.cpp" $(pkg-config --cflags --libs libavformat libavcodec libavutil) \
  -lcurl -lcrypto -lpthread -o "$out"
"$out"
