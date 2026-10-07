#!/usr/bin/env python3
"""One overview picture: a thumbnail of the main state of every screen.

    contact_sheet.py SCREENS_DIR OUT.png
"""
import os
import sys

from PIL import Image, ImageDraw, ImageFont

SRC, OUT = sys.argv[1], sys.argv[2]
SHOTS = [
    ("Board", "board-1-home"),
    ("Discover", "discover-1-home"),
    ("Library", "library-1-home"),
    ("Addons", "addons-1-home"),
    ("Settings", "settings-2-switch"),
    ("Detail: movie", "detail-1-movie-streams"),
    ("Detail: episodes", "detail-3-series-episodes"),
    ("Detail: streams", "detail-4-series-streams"),
    ("Search", "search-1-results"),
    ("Search: nothing found", "search-3-no-results"),
    ("Player", "player-1-controls"),
    ("Player: paused", "player-3-paused"),
    ("Player: buffering", "player-4-buffering"),
    ("Player: tracks", "player-5-track-menu"),
    ("Starting a stream", "launch-1-buffering"),
    ("Dropdown", "overlay-1-dropdown"),
    ("Sign in", "overlay-2-sign-in"),
    ("Message", "overlay-4-toast-error"),
]
W, H, GAP, LABEL = 640, 360, 24, 44
COLS = 3
rows = (len(SHOTS) + COLS - 1) // COLS
sheet = Image.new("RGB", (COLS * W + (COLS + 1) * GAP, rows * (H + LABEL) + (rows + 1) * GAP), (10, 9, 15))
draw = ImageDraw.Draw(sheet)
font = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", 22)
for i, (label, name) in enumerate(SHOTS):
    path = os.path.join(SRC, name + ".png")
    if not os.path.exists(path):
        continue
    img = Image.open(path).convert("RGB").resize((W, H), Image.LANCZOS)
    x = GAP + (i % COLS) * (W + GAP)
    y = GAP + (i // COLS) * (H + LABEL + GAP)
    sheet.paste(img, (x, y))
    draw.text((x + 4, y + H + 8), label, font=font, fill=(235, 232, 250))
sheet.save(OUT)
print("wrote", OUT, sheet.size)
