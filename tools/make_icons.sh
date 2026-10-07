#!/bin/bash
# Builds assets/icons from Stremio's official icon set (stremio-icons, MIT)
# plus our own PlayStation button glyphs, as white (or coloured) PNGs, and
# the search bar and loading dots in assets/images.
#
# Every file is drawn straight from the vector at exactly the size it is
# shown at (the stylesheets' dp size times the UI scale, rounded), so the
# renderer never has to scale it: edges stay as sharp as the pixel grid
# allows. The old files were twice as large and scaled down by the renderer.
#
#   tools/make_icons.sh [ASSETS_DIR]     (needs git, curl and rsvg-convert)
#   SCREEN=2160 tools/make_icons.sh      (for a 4K layout: sizes scale with it)
set -euo pipefail

ASSETS="${1:-$(cd "$(dirname "$0")/../app/assets" && pwd)}"
SCREEN="${SCREEN:-1080}"
# The set for a larger screen goes beside the 1080p one (icons_4k, images_4k):
# the app loads it instead on that screen (src/render_sdl.cpp).
TAG=""
[ "$SCREEN" -gt 1080 ] && TAG="_2k"
[ "$SCREEN" -gt 1440 ] && TAG="_4k"
OUT="$ASSETS/icons$TAG"
IMAGES="$ASSETS/images$TAG"
mkdir -p "$OUT" "$IMAGES"
SRC="${STREMIO_ICONS:-$HOME/stremio-icons}"
[ -d "$SRC/icons" ] || git clone -q --depth 1 https://github.com/Stremio/stremio-icons.git "$SRC"
command -v rsvg-convert >/dev/null || sudo apt-get install -y -qq librsvg2-bin
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# Pixels per dp: kUiScale (src/util.h) times the screen height over 1080.
# ABS is the size relative to 1080p, for sizes known in pixels there.
UI=0.9
SCALE="$(awk -v u="$UI" -v s="$SCREEN" 'BEGIN{printf "%.6f", u*s/1080}')"
ABS="$(awk -v s="$SCREEN" 'BEGIN{printf "%.6f", s/1080}')"
px()  { awk -v d="$1" -v s="$SCALE" 'BEGIN{printf "%d", d*s+0.5}'; }  # dp -> pixels
apx() { awk -v d="$1" -v s="$ABS" 'BEGIN{printf "%d", d*s+0.5}'; }    # css px -> pixels

# Filled icons have no colour (black by default); line icons (checkmark,
# chevrons) draw in currentcolor with fill:none. Make both white; the app
# tints icons with image-color.
cat > "$TMP/white.css" <<'EOF'
svg { color: #ffffff; }
path:not([style*="fill:none"]):not([fill="none"]),
circle:not([style*="fill:none"]):not([fill="none"]),
rect:not([style*="fill:none"]):not([fill="none"]) { fill: #ffffff !important; }
EOF

# stremio_icon <stremio icon> <our file> <size in dp>
stremio_icon() {
  local size
  size="$(px "$3")"
  rsvg-convert -s "$TMP/white.css" -w "$size" -h "$size" "$SRC/icons/$1.svg" -o "$OUT/$2.png"
}

# Menu (42dp): outline icons, the solid one for the page you're on (as
# Stremio's own app draws its menu).
stremio_icon home             nav_board        42
stremio_icon discover         nav_discover     42
stremio_icon library          nav_library      42
stremio_icon addons           nav_addons       42
stremio_icon settings         nav_settings     42
stremio_icon home-outline     nav_board_o      42
stremio_icon discover-outline nav_discover_o   42
stremio_icon library-outline  nav_library_o    42
stremio_icon addons-outline   nav_addons_o     42
stremio_icon settings-outline nav_settings_o   42
stremio_icon search           search           24
stremio_icon play             play             30   # stream rows
stremio_icon play             play_s           22   # "continue" line on the detail page
stremio_icon pause            pause            80
stremio_icon checkmark        check            28   # watched episodes
stremio_icon checkmark        check_s          26   # selected menu entries
stremio_icon chevron-back     chevron_left     28
stremio_icon chevron-forward  chevron_right    28
stremio_icon chevron-down     chevron_down     22

# PlayStation button glyphs (30dp): a dark disc with the symbol in its usual
# colour, like the console's own button prompts.
GLYPH="$(px 30)"
glyph() {
  local out="$1" body="$2"
  cat > "$TMP/$out.svg" <<EOF
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 64 64">
  <circle cx="32" cy="32" r="30" fill="#24222f" stroke="#ffffff" stroke-opacity="0.22" stroke-width="2.5"/>
  $body
</svg>
EOF
  rsvg-convert -w "$GLYPH" -h "$GLYPH" "$TMP/$out.svg" -o "$OUT/$out.png"
}

glyph cross    '<path d="M22 22 L42 42 M42 22 L22 42" stroke="#7fb2f5" stroke-width="5.5" stroke-linecap="round"/>'
glyph circle   '<circle cx="32" cy="32" r="11.5" fill="none" stroke="#f07b7b" stroke-width="5.5"/>'
glyph square   '<rect x="21.5" y="21.5" width="21" height="21" rx="2.5" fill="none" stroke="#dc9bdc" stroke-width="5.5"/>'
glyph triangle '<path d="M32 19 L45 42 L19 42 Z" fill="none" stroke="#5fd3aa" stroke-width="5.5" stroke-linejoin="round"/>'
glyph options  '<path d="M21 24 H43 M21 32 H43 M21 40 H43" stroke="#ffffff" stroke-opacity="0.9" stroke-width="4.5" stroke-linecap="round"/>'

# Shoulder buttons: a pill with the label.
pill() {
  local out="$1" label="$2"
  cat > "$TMP/$out.svg" <<EOF
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 64 64">
  <rect x="3" y="13" width="58" height="38" rx="14" fill="#24222f" stroke="#ffffff" stroke-opacity="0.22" stroke-width="2.5"/>
  <text x="32" y="40.5" text-anchor="middle" font-family="DejaVu Sans" font-weight="bold" font-size="22" fill="#ffffff" fill-opacity="0.92">$label</text>
</svg>
EOF
  rsvg-convert -w "$GLYPH" -h "$GLYPH" "$TMP/$out.svg" -o "$OUT/$out.png"
}
pill l1 L1
pill r1 R1

# D-pad: a rounded plus.
cat > "$TMP/dpad.svg" <<'EOF'
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 64 64">
  <path d="M25 6 h14 a3 3 0 0 1 3 3 v16 h16 a3 3 0 0 1 3 3 v8 a3 3 0 0 1 -3 3 h-16 v16 a3 3 0 0 1 -3 3 h-14 a3 3 0 0 1 -3 -3 v-16 h-16 a3 3 0 0 1 -3 -3 v-8 a3 3 0 0 1 3 -3 h16 v-16 a3 3 0 0 1 3 -3 z"
        fill="#24222f" stroke="#ffffff" stroke-opacity="0.35" stroke-width="2.5" stroke-linejoin="round"/>
  <path d="M32 12 l4 5 h-8 z M32 52 l4 -5 h-8 z M12 32 l5 4 v-8 z M52 32 l-5 4 v-8 z" fill="#ffffff" fill-opacity="0.8"/>
</svg>
EOF
rsvg-convert -w "$GLYPH" -h "$GLYPH" "$TMP/dpad.svg" -o "$OUT/dpad.png"

# Search bar backgrounds at exactly the bar's size: 560x50 dp. The renderer
# draws CSS rounded borders without anti-aliasing; these are smooth.
SB_W="$(px 560)"; SB_H="$(px 50)"
pill_bg() {
  local out="$1" fill_opacity="$2" stroke="$3" rw rh rr
  rw="$(awk -v v="$SB_W" 'BEGIN{printf "%.2f", v-2.5}')"
  rh="$(awk -v v="$SB_H" 'BEGIN{printf "%.2f", v-2.5}')"
  rr="$(awk -v v="$SB_H" 'BEGIN{printf "%.2f", (v-2.5)/2}')"
  cat > "$TMP/$out.svg" <<EOF
<svg xmlns="http://www.w3.org/2000/svg" width="$SB_W" height="$SB_H" viewBox="0 0 $SB_W $SB_H">
  <rect x="1.25" y="1.25" width="$rw" height="$rh" rx="$rr" fill="#ffffff" fill-opacity="$fill_opacity" $stroke/>
</svg>
EOF
  rsvg-convert -w "$SB_W" -h "$SB_H" "$TMP/$out.svg" -o "$IMAGES/$out.png"
}
pill_bg searchbar     0.08 ''
pill_bg searchbar_sel 0.14 'stroke="#ffffff" stroke-width="2.25"'

# Logo (homarr dashboard-icons, vector): in the menu (60dp), on the sign-in
# card (72dp) and while a stream starts (96dp).
curl -sfL -o "$TMP/logo.svg" https://cdn.jsdelivr.net/gh/homarr-labs/dashboard-icons/svg/stremio.svg
for l in "logo 60" "logo_m 72" "logo_l 96"; do
  set -- $l
  rsvg-convert -w "$(px "$2")" -h "$(px "$2")" "$TMP/logo.svg" -o "$OUT/$1.png"
done
# The boot screen's mark: 144 px on the 1920-wide layout, whatever the screen (not the old app's 0.9 scale).
rsvg-convert -w "$((144 * SCREEN / 1080))" -h "$((144 * SCREEN / 1080))" "$TMP/logo.svg" -o "$OUT/logo_xl.png"

# Loading dots (shown at 11 and 16 px, see theme.rcss / watch.rcss).
cat > "$TMP/dot.svg" <<'EOF'
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 64 64">
  <circle cx="32" cy="32" r="32" fill="#8d70ff"/>
</svg>
EOF
for d in 11 16; do
  rsvg-convert -w "$(apx $d)" -h "$(apx $d)" "$TMP/dot.svg" -o "$IMAGES/dot$d.png"
done

# The full-screen backgrounds and shades are gradients, made by make_images.py.

echo "icons written to $OUT"
ls -la "$OUT"
