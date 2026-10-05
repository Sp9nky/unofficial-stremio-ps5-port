#!/bin/bash
# Builds assets/icons from Stremio's official icon set (stremio-icons, MIT)
# plus our own PlayStation button glyphs, as white (or coloured) PNGs at
# twice the size they're drawn, so the renderer's linear filtering scales
# them down cleanly.
#
#   tools/make_icons.sh [ASSETS_DIR]     (needs git and rsvg-convert)
set -euo pipefail

ASSETS="${1:-$(cd "$(dirname "$0")/../app/assets" && pwd)}"
OUT="$ASSETS/icons"
SRC="${STREMIO_ICONS:-$HOME/stremio-icons}"
[ -d "$SRC/icons" ] || git clone -q --depth 1 https://github.com/Stremio/stremio-icons.git "$SRC"
command -v rsvg-convert >/dev/null || sudo apt-get install -y -qq librsvg2-bin
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# Stremio icon -> our file, pixel size (2x the CSS size it's shown at).
# Filled icons have no colour (black by default); line icons (checkmark,
# chevrons) draw in currentcolor with fill:none. Make both white.
cat > "$TMP/white.css" <<'EOF'
svg { color: #ffffff; }
path:not([style*="fill:none"]):not([fill="none"]),
circle:not([style*="fill:none"]):not([fill="none"]),
rect:not([style*="fill:none"]):not([fill="none"]) { fill: #ffffff !important; }
EOF
stremio_icon() {
  local name="$1" out="$2" size="$3"
  # All white (the app tints icons with image-color).
  rsvg-convert -s "$TMP/white.css" -w "$size" -h "$size" "$SRC/icons/$name.svg" -o "$OUT/$out.png"
}

# Menu: outline icons, the solid one for the page you're on (as Stremio's
# own app draws its menu).
stremio_icon home             nav_board     84
stremio_icon discover         nav_discover  84
stremio_icon library          nav_library   84
stremio_icon addons           nav_addons    84
stremio_icon settings         nav_settings  84
stremio_icon home-outline     nav_board_o     84
stremio_icon discover-outline nav_discover_o  84
stremio_icon library-outline  nav_library_o   84
stremio_icon addons-outline   nav_addons_o    84
stremio_icon settings-outline nav_settings_o  84
stremio_icon search          search        48
stremio_icon play            play          60
stremio_icon pause           pause         160
stremio_icon checkmark       check         56
stremio_icon chevron-back    chevron_left  56
stremio_icon chevron-forward chevron_right 56
stremio_icon chevron-down    chevron_down  56

# PlayStation button glyphs (shown at 30px): a dark disc with the symbol in
# its usual colour, like the console's own button prompts.
glyph() {
  local out="$1" body="$2"
  cat > "$TMP/$out.svg" <<EOF
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 64 64">
  <circle cx="32" cy="32" r="30" fill="#24222f" stroke="#ffffff" stroke-opacity="0.22" stroke-width="2.5"/>
  $body
</svg>
EOF
  rsvg-convert -w 60 -h 60 "$TMP/$out.svg" -o "$OUT/$out.png"
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
  rsvg-convert -w 60 -h 60 "$TMP/$out.svg" -o "$OUT/$out.png"
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
rsvg-convert -w 60 -h 60 "$TMP/dpad.svg" -o "$OUT/dpad.png"

# Search bar backgrounds at exactly the bar's pixel size: 560x50 dp in
# shell.rcss times kUiScale (0.9, src/util.h) = 504x45. The renderer draws
# CSS rounded borders without anti-aliasing; these are smooth.
pill_bg() {
  local out="$1" fill_opacity="$2" stroke="$3"
  cat > "$TMP/$out.svg" <<EOF
<svg xmlns="http://www.w3.org/2000/svg" width="504" height="45" viewBox="0 0 504 45">
  <rect x="1.25" y="1.25" width="501.5" height="42.5" rx="21.25" fill="#ffffff" fill-opacity="$fill_opacity" $stroke/>
</svg>
EOF
  rsvg-convert -w 504 -h 45 "$TMP/$out.svg" -o "$ASSETS/images/$out.png"
}
pill_bg searchbar     0.08 ''
pill_bg searchbar_sel 0.14 'stroke="#ffffff" stroke-width="2.25"'

# Logo (homarr dashboard-icons, vector) at exactly its drawn size: 60dp in
# shell.rcss times kUiScale = 54px, so it isn't resampled.
curl -sfL -o "$TMP/logo.svg" https://cdn.jsdelivr.net/gh/homarr-labs/dashboard-icons/svg/stremio.svg
rsvg-convert -w 54 -h 54 "$TMP/logo.svg" -o "$OUT/logo.png"

# Detail page shade over the blurred backdrop: dark enough on the left for
# the text, letting the picture show through elsewhere.
cat > "$TMP/shade.svg" <<'EOF'
<svg xmlns="http://www.w3.org/2000/svg" width="960" height="540" viewBox="0 0 960 540">
  <defs>
    <linearGradient id="h" x1="0" x2="1" y1="0" y2="0">
      <stop offset="0" stop-color="#08070d" stop-opacity="0.82"/>
      <stop offset="0.35" stop-color="#08070d" stop-opacity="0.62"/>
      <stop offset="0.6" stop-color="#08070d" stop-opacity="0.25"/>
      <stop offset="1" stop-color="#08070d" stop-opacity="0.15"/>
    </linearGradient>
    <linearGradient id="v" x1="0" x2="0" y1="0" y2="1">
      <stop offset="0.7" stop-color="#08070d" stop-opacity="0"/>
      <stop offset="1" stop-color="#08070d" stop-opacity="0.55"/>
    </linearGradient>
  </defs>
  <rect width="960" height="540" fill="url(#h)"/>
  <rect width="960" height="540" fill="url(#v)"/>
</svg>
EOF
rsvg-convert -w 960 -h 540 "$TMP/shade.svg" -o "$ASSETS/images/shade_detail.png"

# Loading dots (shown at 11px and 16px, see theme.rcss / watch.rcss).
dot() {
  local size="$1"
  cat > "$TMP/dot.svg" <<'EOF'
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 64 64">
  <circle cx="32" cy="32" r="30" fill="#8d70ff"/>
</svg>
EOF
  rsvg-convert -w "$size" -h "$size" "$TMP/dot.svg" -o "$ASSETS/images/dot$size.png"
}
dot 22
dot 32

echo "icons written to $OUT"
ls -la "$OUT"
