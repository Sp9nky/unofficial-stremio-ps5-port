#!/usr/bin/env bash
# The artwork pipeline (gl/art.cpp, gl/gpu_upload.cpp) against a pretend image host. Run in the build VM.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
port=${ART_PORT:-18996}
python3 "$root/tools/tests/mock_art_server.py" "$port" &
mock=$!
trap 'kill $mock 2>/dev/null || true' EXIT
for _ in $(seq 1 50); do (echo > /dev/tcp/127.0.0.1/$port) 2>/dev/null && break; sleep 0.1; done
ART_PORT=$port PREVIEW_BUILD=${PREVIEW_BUILD:-$HOME/preview-build} bash "$root/tools/preview/preview.sh" /tmp/art-shots artcache
