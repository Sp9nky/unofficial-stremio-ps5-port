#!/usr/bin/env bash
# Packs dist/PPSA77711 into one image file, dist/PPSA77711.ffpfsc, which
# ShadowMountPlus mounts as the app. File permissions are stored inside the
# image, so a copy tool can't strip them (PS5 Upload drops the execute bit
# from loose files, and the PS5 then refuses to start eboot.bin).
#
#   native/pack.sh [TITLE_ID]
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
TITLE="${1:-PPSA77711}"
BP=${BOILERPLATE_DIR:-/root/ps5-native-app-boilerplate}
SRC="$ROOT/dist/$TITLE"
OUT="$ROOT/dist/$TITLE.ffpfsc"

[ -d "$SRC" ] || { echo "build first: $SRC is missing"; exit 1; }
MKPFS="$(cd "$BP" && bash tools/setup-packaging-dependencies.sh ffpfsc)"

# Pack from a Linux copy with every file and folder 0777, like the folders
# that start fine on the console.
STAGE=/root/stremio-pack/$TITLE
rm -rf /root/stremio-pack && mkdir -p "$STAGE"
cp -r "$SRC/." "$STAGE/"
chmod -R 0777 "$STAGE"

rm -f "$OUT"
"$MKPFS" pack folder --no-adjust-output-file-extension --version PS5 --verify "$STAGE" "$OUT"
echo "Packed $OUT ($(stat -c %s "$OUT") bytes)"
