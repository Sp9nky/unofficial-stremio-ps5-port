#!/usr/bin/env bash
# Builds Stremio as one native PS5 app: the title's own eboot.bin, no
# launcher, no ELF loader, no app swap. Run inside WSL:
#
#   native/build.sh [TITLE_ID] [NAME]      default PPSA77711 "Stremio"
#
# Output: dist/<TITLE_ID>/ (and .zip), the folder for /data/homebrew.
#
# How: CMake compiles Stremio (PS5_NATIVE=ON) into libstremio.a with the
# payload SDK's compiler; it's linked here with the full C++ runtime and the
# console fixes in native/, then ps5-native-app-boilerplate's converter and
# signer make the eboot.bin.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
TITLE="${1:-PPSA77711}"
NAME="${2:-Stremio}"
APP_FILES="${APP_FILES:-$ROOT/app}"   # assets, fonts, ca-bundle.crt, sce_sys
SDK=/opt/ps5-payload-sdk
HB=$SDK/target/user/homebrew/lib
BP=${BOILERPLATE_DIR:-/root/ps5-native-app-boilerplate}
TOOL=$BP/build/host/ps5-native-tool
B=/root/stremio-native-build
OUT=${OUT_DIR:-$ROOT/dist}

[ -x "$TOOL" ] || (cd "$BP" && bash tools/build-host-tools.sh)

echo ">> compiling Stremio"
rm -rf "$B" && mkdir -p "$B"
"$SDK/bin/prospero-cmake" -S "$ROOT" -B "$B/cmake" -DCMAKE_BUILD_TYPE=Release -DPS5_NATIVE=ON >/dev/null
make -C "$B/cmake" -j"$(nproc)" 2>&1 | grep -E 'error|warning: unused' || true
[ -f "$B/cmake/libstremio.a" ] || { echo "compile failed"; exit 1; }

CC="$SDK/bin/prospero-clang"
"$SDK/bin/prospero-clang++" -std=c++20 -O2 -fno-exceptions -fno-rtti -c "$BP/tooling/native/app_crt.cpp" -o "$B/app_crt.o"
for f in shims console_curl ps5_modules heap posix_fixes; do
	"$CC" -O2 -I"$SDK/target/user/homebrew/include" -c "$HERE/$f.c" -o "$B/$f.o"
done

echo ">> linking"
# Every library Stremio uses, in static form (stub .so for system modules).
read -r -a DEPS <<< "$("$SDK/bin/prospero-pkg-config" --static --libs \
	sdl2 freetype2 libavformat libavcodec libavutil libswresample libswscale libcurl libwebp |
	tr ' ' '\n' | grep -E '^-[lL]' | grep -v -E '^(-lm|-lc)$' | tr '\n' ' ')"
"$SDK/bin/prospero-lld" -T "$HERE/ps5-app.ld" --eh-frame-hdr \
	--version-script "$BP/tooling/native/app-symbols.map" -e _start -o "$B/pie.elf" \
	--wrap=fcntl -L "$SDK/target/lib" -L "$HB" \
	`# All allocation goes through native/heap.c: a dlmalloc heap in direct` \
	`# memory (the system heap only has a few MB of flexible memory left).` \
	`# native/posix_fixes.c: 8 MB thread stacks, pipe() as a socket pair.` \
	--wrap=pthread_create --wrap=pipe \
	--wrap=malloc --wrap=free --wrap=calloc --wrap=realloc --wrap=reallocf \
	--wrap=posix_memalign --wrap=memalign --wrap=aligned_alloc --wrap=valloc \
	--wrap=malloc_usable_size \
	`# console_curl.c and the payload SDK's libc.a both define name lookup,` \
	`# pipe2, gmtime_r, ...: the first (console_curl.c, proven in the sandbox) wins.` \
	--allow-multiple-definition \
	`# Optional (weak) hooks with no native provider: absent, so their users` \
	`# take their own fallback (libc++abi's thread-exit list; termios).` \
	--defsym=__cxa_thread_atexit_impl=0 --defsym=__syscall=0 \
	"$B/app_crt.o" "$B/shims.o" "$B/console_curl.o" "$B/ps5_modules.o" "$B/heap.o" "$B/posix_fixes.o" \
	--whole-archive "$B/cmake/libstremio.a" --no-whole-archive \
	--start-group \
	"$HB/librmlui.a" "${DEPS[@]}" \
	"$SDK/target/lib/libc++.a" "$SDK/target/lib/libc++abi.a" "$SDK/target/lib/libunwind.a" \
	"$SDK/target/lib/libc.a" \
	--end-group \
	--as-needed "$SDK"/target/lib/*.so

# A function that resolves to a module the PS5 doesn't load into an app is a
# call to address 0 at run time. List them (they need a definition in native/).
python3 "$HERE/check_imports.py" "$B/pie.elf" "$SDK/target/lib"

"$TOOL" link --in "$B/pie.elf" --out "$B/eboot.elf" --stub-dir "$SDK/target/lib" \
	--module-sdk 0x02000009 --companion-sdk 0x08050001 --file-name eboot.elf >/dev/null

echo ">> packaging $TITLE"
APP=$OUT/$TITLE
rm -rf "$APP" "$OUT/$TITLE.zip"
mkdir -p "$APP/sce_sys" "$APP/sce_module"
"$TOOL" self --sign --in "$B/eboot.elf" --out "$APP/eboot.bin" --magic 0x1D3D154F >/dev/null
cp "$BP/runtime/libc.prx" "$APP/sce_module/libc.prx"  # the boilerplate's loader shim, already signed
python3 - "$APP_FILES/sce_sys/param.json" "$APP/sce_sys/param.json" "$TITLE" "$NAME" <<'PY'
import json, sys
p = json.load(open(sys.argv[1]))
title = sys.argv[3]
if title != "PPSA77711":
    p["titleId"] = title
    p["conceptId"] = title[4:]
    p["contentId"] = "UP9000-%s_00-STREMIOPS5TEST%s" % (title, title[-2:])  # 16 characters
p["downloadDataSize"] = 2048   # MB of app storage: settings, artwork cache, log
p["localizedParameters"]["en-US"]["titleName"] = sys.argv[4]
json.dump(p, open(sys.argv[2], "w"), indent=2)
PY
cp "$APP_FILES/sce_sys/icon0.png" "$APP/sce_sys/"
# Home-screen background (pic0) and launch background (pic1): 3840x2160
# BC7 DDS, made from sce_sys/pic0-source.png and pic1-source.png by
# tools/make_dds.ps1.
for pic in pic0.dds pic1.dds; do
	[ -f "$APP_FILES/sce_sys/$pic" ] && cp "$APP_FILES/sce_sys/$pic" "$APP/sce_sys/"
done
bash "$BP/tools/validate-assets.sh" "$APP/sce_sys" >/dev/null
cp -r "$APP_FILES/assets" "$APP_FILES/fonts" "$APP_FILES/ca-bundle.crt" "$APP/"
# License notices travel with the app: the GPL, the component list, and the
# license text of everything it contains (app/licenses/README.txt).
cp -r "$APP_FILES/licenses" "$APP/"
cp "$ROOT/LICENSE" "$ROOT/THIRD_PARTY.md" "$APP/"
(cd "$OUT" && zip -qr "$TITLE.zip" "$TITLE")
echo "Built $APP ($(stat -c %s "$APP/eboot.bin") byte eboot.bin)"
