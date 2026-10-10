#!/usr/bin/env bash
# Offline test of Real-Debrid / Torbox handling: a pretend server for both, and src/debrid.cpp against it.
# Run it where clang++ and libcurl are (the build VM):  bash tools/tests/run_debrid_test.sh
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
port=${PORT:-18999}
out=${TMPDIR:-/tmp}/debrid_test_bin
${CXX:-clang++-18} -std=c++20 -I"$root/src" -I"$root/third_party" "$root/tools/tests/debrid_test.cpp" "$root/src/debrid.cpp" \
  "$root/src/http.cpp" "$root/src/util.cpp" -lcurl -lcrypto -lpthread -o "$out"
python3 "$root/tools/tests/mock_debrid_server.py" "$port" &
mock=$!
trap 'kill $mock 2>/dev/null || true' EXIT
for _ in $(seq 1 50); do (echo > /dev/tcp/127.0.0.1/$port) 2>/dev/null && break; sleep 0.1; done
"$out" "$port"
