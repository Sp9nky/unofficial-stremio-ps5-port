#!/usr/bin/env bash
# A host that answers some pieces with HTTP 429, and NetStream reading through it. Run where clang++, libcurl and FFmpeg are.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
port=${PORT:-18998}
out=${TMPDIR:-/tmp}/netstream_test_bin
${CXX:-clang++-18} -std=c++20 -I"$root/src" -I"$root/third_party" "$root/tools/tests/netstream_test.cpp" "$root/src/netstream.cpp" \
  "$root/src/http.cpp" "$root/src/util.cpp" $(pkg-config --cflags --libs libavformat libavcodec libavutil) -lcurl -lcrypto -lpthread -o "$out"
python3 "$root/tools/tests/mock_range_server.py" "$port" &
mock=$!
trap 'kill $mock 2>/dev/null || true' EXIT
for _ in $(seq 1 50); do (echo > /dev/tcp/127.0.0.1/$port) 2>/dev/null && break; sleep 0.1; done
"$out" "$port"
