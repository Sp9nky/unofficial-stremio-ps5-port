#!/usr/bin/env python3
"""Draws the sample artwork for the UI previews: invented titles, generated
posters (2:3) and backdrops (16:9). No real film or series is shown anywhere.

    make_sample_art.py OUT_DIR FONT.ttf
"""
import math
import os
import sys

from PIL import Image, ImageDraw, ImageFilter, ImageFont

OUT, FONT = sys.argv[1], sys.argv[2]
os.makedirs(OUT, exist_ok=True)

# name, top colour, bottom colour, accent, motif
TITLES = [
    ("The Lantern Keepers", "#1b2a5c", "#0b1022", "#ffb347", "sun"),
    ("Salt and Static", "#3b1d4f", "#0f0a1c", "#ff6f91", "waves"),
    ("Midnight Ferry", "#0f3d4a", "#06141a", "#7de3d1", "moon"),
    ("Orchard Street", "#4a2a12", "#150c05", "#ffcf6e", "tower"),
    ("Northbound", "#1d3b2d", "#08130d", "#a8f0c0", "peaks"),
    ("Paper Moons", "#4b2c6b", "#140b22", "#e7c6ff", "moon"),
    ("The Quiet Meridian", "#16304f", "#070e19", "#8ec5ff", "rings"),
    ("Glasshouse", "#12443f", "#061715", "#9fffe0", "grid"),
    ("Ember Lines", "#5a1f1a", "#190807", "#ff9a6a", "stripes"),
    ("Harbor Lights", "#23284f", "#0a0c1c", "#ffd36e", "tower"),
    ("A Slow Country", "#3a3322", "#12100a", "#e6d6a0", "peaks"),
    ("Velvet Hours", "#4f1d3d", "#17080f", "#ff8fc3", "sun"),
]


def hexrgb(h):
    h = h.lstrip("#")
    return tuple(int(h[i:i + 2], 16) for i in (0, 2, 4))


def gradient(w, h, top, bottom):
    img = Image.new("RGB", (w, h))
    px = img.load()
    for y in range(h):
        t = y / (h - 1)
        c = tuple(int(top[i] + (bottom[i] - top[i]) * t) for i in range(3))
        for x in range(w):
            px[x, y] = c
    return img


def motif(draw, w, h, kind, accent, scale=1.0):
    a = hexrgb(accent)
    cx, cy = w * 0.5, h * 0.42
    r = min(w, h) * 0.26 * scale
    if kind == "sun":
        for i in range(5):
            draw.ellipse([cx - r - i * 14, cy - r - i * 14, cx + r + i * 14, cy + r + i * 14],
                         outline=a + (60 - i * 10,), width=3)
        draw.ellipse([cx - r, cy - r, cx + r, cy + r], fill=a + (230,))
        for k in range(6):
            y = cy + r * 0.15 + k * r * 0.17
            draw.rectangle([0, y, w, y + 5 + k * 2], fill=hexrgb("#000000") + (140,))
    elif kind == "moon":
        draw.ellipse([cx - r, cy - r, cx + r, cy + r], fill=a + (235,))
        draw.ellipse([cx - r * 0.6, cy - r * 1.1, cx + r * 1.4, cy + r * 0.9], fill=(0, 0, 0, 170))
        for k in range(18):
            sx = (k * 97) % w
            sy = (k * 53) % int(h * 0.5)
            draw.ellipse([sx, sy, sx + 4, sy + 4], fill=(255, 255, 255, 190))
    elif kind == "peaks":
        for i, (px_, hh, al) in enumerate([(0.2, 0.5, 120), (0.55, 0.62, 170), (0.85, 0.42, 120)]):
            x0 = w * px_
            top = h * (0.85 - hh)
            draw.polygon([(x0 - w * 0.4, h * 0.85), (x0, top), (x0 + w * 0.4, h * 0.85)], fill=a + (al,))
        draw.ellipse([w * 0.7, h * 0.12, w * 0.7 + r * 0.5, h * 0.12 + r * 0.5], fill=(255, 255, 255, 220))
    elif kind == "rings":
        for i in range(7):
            rr = r * (0.3 + i * 0.22)
            draw.ellipse([cx - rr, cy - rr, cx + rr, cy + rr], outline=a + (220 - i * 28,), width=5)
        draw.ellipse([cx - 14, cy - 14, cx + 14, cy + 14], fill=a + (255,))
    elif kind == "stripes":
        for i in range(-3, 9):
            x = i * w / 7.0
            draw.polygon([(x, h), (x + 34, h), (x + 34 + h * 0.5, 0), (x + h * 0.5, 0)],
                         fill=a + (60 + (i % 3) * 45,))
    elif kind == "waves":
        for k in range(8):
            pts = []
            for x in range(0, int(w) + 8, 8):
                pts.append((x, h * 0.2 + k * h * 0.065 + math.sin(x / w * 6.28 * 1.5 + k * 0.7) * h * 0.03))
            draw.line(pts, fill=a + (210 - k * 20,), width=6)
    elif kind == "grid":
        step = w / 9.0
        for gx in range(10):
            for gy in range(8):
                rad = 4 + 7 * (0.5 + 0.5 * math.sin(gx * 0.9 + gy * 0.7))
                draw.ellipse([gx * step + step * 0.3 - rad, h * 0.1 + gy * step * 0.8 - rad,
                              gx * step + step * 0.3 + rad, h * 0.1 + gy * step * 0.8 + rad], fill=a + (200,))
    elif kind == "tower":
        base = h * 0.7
        for i in range(9):
            bw = w * (0.08 + (i * 37 % 7) * 0.01)
            bx = i * w / 8.5
            bh = h * (0.18 + ((i * 53) % 9) * 0.05)
            draw.rectangle([bx, base - bh, bx + bw, base], fill=a + (70 + (i % 3) * 40,))
            for wy in range(int(base - bh + 14), int(base - 10), 22):
                for wx in range(int(bx + 8), int(bx + bw - 8), 16):
                    if (wx * wy) % 5 < 3:
                        draw.rectangle([wx, wy, wx + 6, wy + 9], fill=a + (230,))
        draw.ellipse([w * 0.1, h * 0.1, w * 0.1 + r * 0.7, h * 0.1 + r * 0.7], fill=a + (200,))


def poster(i, name, top, bottom, accent, kind):
    S = 2
    w, h = 400 * S, 600 * S
    img = gradient(w, h, hexrgb(top), hexrgb(bottom)).convert("RGBA")
    layer = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    motif(ImageDraw.Draw(layer), w, h, kind, accent)
    img = Image.alpha_composite(img, layer)
    # darken the bottom third for the title
    shade = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    sp = shade.load()
    for y in range(int(h * 0.55), h):
        t = (y - h * 0.55) / (h * 0.45)
        for x in range(w):
            sp[x, y] = (0, 0, 0, int(190 * t * t))
    img = Image.alpha_composite(img, shade)
    d = ImageDraw.Draw(img)
    font = ImageFont.truetype(FONT, 46 * S)
    words, lines, cur = name.split(), [], ""
    for wd in words:
        trial = (cur + " " + wd).strip()
        if d.textlength(trial, font=font) > w - 70 * S and cur:
            lines.append(cur)
            cur = wd
        else:
            cur = trial
    lines.append(cur)
    y = h - 60 * S - len(lines) * 56 * S
    for line in lines:
        d.text((36 * S, y), line, font=font, fill=(255, 255, 255, 245))
        y += 56 * S
    img = img.convert("RGB").resize((400, 600), Image.LANCZOS)
    img.save(os.path.join(OUT, "poster%02d.png" % i))


def backdrop(i, name, top, bottom, accent, kind):
    S = 1
    w, h = 1280, 720
    img = gradient(w, h, hexrgb(top), hexrgb(bottom)).convert("RGBA")
    layer = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    motif(ImageDraw.Draw(layer), w, h, kind, accent, scale=1.5)
    layer = layer.filter(ImageFilter.GaussianBlur(1.2))
    img = Image.alpha_composite(img, layer)
    img = img.convert("RGB")
    img.save(os.path.join(OUT, "backdrop%02d.png" % i))


for i, t in enumerate(TITLES):
    poster(i, *t)
    backdrop(i, *t)
def qr_placeholder():
    """A picture that looks like a QR code and encodes nothing: for layouts only."""
    import random
    rnd = random.Random(7)
    n, m = 33, 12
    img = Image.new("RGB", (n * m, n * m), (255, 255, 255))
    d = ImageDraw.Draw(img)

    def cell(x, y):
        d.rectangle([x * m, y * m, x * m + m - 1, y * m + m - 1], fill=(14, 12, 20))

    for x in range(n):
        for y in range(n):
            if rnd.random() < 0.47:
                cell(x, y)
    for ox, oy in [(0, 0), (n - 7, 0), (0, n - 7)]:
        d.rectangle([(ox - 1) * m, (oy - 1) * m, (ox + 8) * m - 1, (oy + 8) * m - 1], fill=(255, 255, 255))
        d.rectangle([ox * m, oy * m, (ox + 7) * m - 1, (oy + 7) * m - 1], fill=(14, 12, 20))
        d.rectangle([(ox + 1) * m, (oy + 1) * m, (ox + 6) * m - 1, (oy + 6) * m - 1], fill=(255, 255, 255))
        d.rectangle([(ox + 2) * m, (oy + 2) * m, (ox + 5) * m - 1, (oy + 5) * m - 1], fill=(14, 12, 20))
    img.save(os.path.join(OUT, "qr.png"))


qr_placeholder()
print("wrote", len(TITLES), "posters and backdrops to", OUT)
