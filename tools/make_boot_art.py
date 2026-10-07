#!/usr/bin/env python3
"""The boot screen's name and the console's start-up pictures (needs Pillow).

  tools/make_boot_art.py name                      app/assets/icons_4k/name.png: "Stremio" as the boot
                                                   screen shows it (Inter SemiBold, 38 px on the 1920-wide
                                                   layout, letters 3 px apart, 72 % white), at 4K size
  tools/make_boot_art.py pictures <bootlight.png>  app/sce_sys/pic0-source.png and pic1-source.png: the
                                                   page's light (a picture of it, made by the preview
                                                   tool's `boot` scene), the logo and the name, 3840x2160,
                                                   exactly where the boot screen has them. Then
                                                   tools/make_dds.ps1 turns them into what the console reads.

The name is a picture so that it can be drawn from the first frame, before the fonts are loaded, and so that
the start-up pictures and the first frame of the boot screen are the same.
"""
import math
import os
import sys

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.normpath(os.path.join(os.path.dirname(__file__), ".."))
ICONS = os.path.join(ROOT, "app", "assets", "icons_4k")
SCENE_W, SCENE_H = 1920, 1080
CX, CY = 960.0, 470.0          # the logo's centre in the 1920x1080 layout
NAME_BASELINE = CY + 156.0     # where the name sits
SIZE, TRACKING = 76, 6         # 38 px and 3 px, at 4K
CANVAS_W, CANVAS_H, BASE = 700, 130, 90  # the picture of the name: its size, and the baseline in it (the name is
                                         # centred in it, so the app can draw it at a fixed size)


def name_image():
    font = ImageFont.truetype(os.path.join(ROOT, "app", "fonts", "Inter-SemiBold.ttf"), SIZE)
    word = "Stremio"
    advances = [font.getlength(c) for c in word]
    total = sum(advances) + TRACKING * (len(word) - 1)
    img = Image.new("RGBA", (CANVAS_W, CANVAS_H), (255, 255, 255, 0))
    layer = Image.new("L", (CANVAS_W, CANVAS_H), 0)
    draw = ImageDraw.Draw(layer)
    x = (CANVAS_W - total) / 2.0
    for c, a in zip(word, advances):
        draw.text((x, BASE), c, font=font, fill=255, anchor="ls")
        x += a + TRACKING
    alpha = layer.point(lambda v: int(v * 0.72))
    img.putalpha(alpha)
    return img


def make_name():
    img = name_image()
    img.save(os.path.join(ICONS, "name.png"), optimize=True)
    print("name.png", img.size)


def make_pictures(light_path):
    light = Image.open(light_path).convert("RGB").resize((2 * SCENE_W, 2 * SCENE_H), Image.LANCZOS)
    logo = Image.open(os.path.join(ICONS, "logo_xl.png")).convert("RGBA")  # 288 px = 144 px at 4K
    name = Image.open(os.path.join(ICONS, "name.png")).convert("RGBA")
    light = light.convert("RGBA")
    light.alpha_composite(logo, (int(2 * CX - logo.width / 2), int(2 * CY - logo.height / 2)))
    light.alpha_composite(name, (int(2 * CX - name.width / 2), int(2 * NAME_BASELINE - BASE)))
    for n in ("pic0-source.png", "pic1-source.png"):
        light.convert("RGB").save(os.path.join(ROOT, "app", "sce_sys", n))
    print("pic0-source.png, pic1-source.png", light.size)


if __name__ == "__main__":
    if len(sys.argv) >= 2 and sys.argv[1] == "name":
        make_name()
    elif len(sys.argv) >= 3 and sys.argv[1] == "pictures":
        make_pictures(sys.argv[2])
    else:
        print(__doc__)
        sys.exit(1)
