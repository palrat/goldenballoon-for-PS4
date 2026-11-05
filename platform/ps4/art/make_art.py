#!/usr/bin/env python3
"""Draw the PS4 menu tile (icon0.png, 512x512) and launch screen (pic1.png,
1920x1080) from scratch, so the package carries no third-party artwork.

usage: python3 make_art.py <output dir>       (needs Pillow and Noto Sans Black)

Motif: a golden balloon over a sunny island race track with a chequered flag.
"""
import math
import sys
from PIL import Image, ImageDraw, ImageFilter, ImageFont

FONT_PATHS = ["/usr/share/fonts/noto/NotoSans-Black.ttf",
              "/usr/share/fonts/truetype/noto/NotoSans-Black.ttf"]
RED, YELLOW, BLUE, DARK = (230, 38, 30), (255, 212, 0), (24, 118, 222), (40, 20, 10)


def font(size):
    for p in FONT_PATHS:
        try:
            return ImageFont.truetype(p, size)
        except OSError:
            pass
    sys.exit("Noto Sans Black not found (install the noto-fonts package)")


def sky(w, h):
    img = Image.new("RGB", (w, h))
    d = ImageDraw.Draw(img)
    for y in range(h):
        t = y / h
        d.line([(0, y), (w, y)], fill=(int(22 + 120 * t), int(110 + 100 * t), int(215 + 40 * t)))
    return img


def rays(img, cx, cy, n=18):
    layer = Image.new("RGBA", img.size, (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    r = max(img.size) * 1.5
    for i in range(n):
        a0 = 2 * math.pi * i / n
        a1 = a0 + math.pi / n
        d.polygon([(cx, cy), (cx + r * math.cos(a0), cy + r * math.sin(a0)),
                   (cx + r * math.cos(a1), cy + r * math.sin(a1))], fill=(255, 255, 255, 34))
    img.paste(layer, (0, 0), layer)


def hills(img, base):
    w, h = img.size
    d = ImageDraw.Draw(img)
    d.ellipse([-0.3 * w, base - 0.10 * h, 0.65 * w, base + 0.55 * h], fill=(70, 175, 60))
    d.ellipse([0.35 * w, base - 0.06 * h, 1.3 * w, base + 0.6 * h], fill=(58, 160, 52))
    d.rectangle([0, base + 0.12 * h, w, h], fill=(48, 140, 45))
    # sandy track with red/white kerb
    d.ellipse([-0.2 * w, base + 0.16 * h, 1.2 * w, base + 0.9 * h], fill=(236, 205, 140))
    seg = max(12, w // 40)
    for i, x in enumerate(range(-seg, w + seg, seg)):
        d.rectangle([x, base + 0.15 * h, x + seg, base + 0.15 * h + seg // 2],
                    fill=RED if i % 2 else (255, 255, 255))


def trees(img, xs, base, s):
    d = ImageDraw.Draw(img)
    for x in xs:
        d.rectangle([x - s * 0.08, base - s * 0.6, x + s * 0.08, base], fill=(120, 70, 30))
        d.ellipse([x - s * 0.4, base - s * 1.25, x + s * 0.4, base - s * 0.45], fill=(40, 125, 40))
        d.ellipse([x - s * 0.22, base - s * 1.1, x + s * 0.05, base - s * 0.85], fill=(95, 190, 80))


def balloon(img, cx, cy, r):
    """Teardrop balloon with a knot, like the collectible balloons (no string)."""
    def outline(scale, ox=0, oy=0):
        pts = []
        for i in range(120):
            t = 2 * math.pi * i / 120
            x = math.sin(t)
            y = -math.cos(t)
            # narrower towards the bottom: a teardrop
            x *= 1.0 - 0.28 * max(0.0, y) ** 1.6
            pts.append((cx + ox + x * r * scale, cy + oy + y * r * 1.15 * scale))
        return pts
    shadow = Image.new("RGBA", img.size, (0, 0, 0, 0))
    ImageDraw.Draw(shadow).polygon(outline(1.0, r * 0.08, r * 0.12), fill=(0, 0, 0, 80))
    img.paste(shadow.filter(ImageFilter.GaussianBlur(r * 0.08)), (0, 0), shadow.filter(ImageFilter.GaussianBlur(r * 0.08)))
    d = ImageDraw.Draw(img)
    d.polygon(outline(1.0), fill=(170, 100, 0))
    for k in range(40, 0, -1):
        t = k / 40
        col = (255, int(150 + 95 * (1 - t)), int(10 + 120 * (1 - t) ** 2))
        d.polygon(outline(0.94 * t, -r * 0.18 * (1 - t), -r * 0.22 * (1 - t)), fill=col)
    d.ellipse([cx - r * 0.55, cy - r * 0.85, cx - r * 0.2, cy - r * 0.35], fill=(255, 252, 225))
    ky = cy + r * 1.12
    d.polygon([(cx - r * 0.16, ky + r * 0.16), (cx + r * 0.16, ky + r * 0.16), (cx, ky - r * 0.05)],
              fill=(214, 135, 0), outline=(150, 85, 0))


def flag(img, x, y, s, ground):
    d = ImageDraw.Draw(img)
    d.ellipse([x - s * 0.22, ground - s * 0.05, x + s * 0.22, ground + s * 0.05], fill=(35, 110, 35))
    d.rectangle([x - s * 0.04, y, x + s * 0.04, ground], fill=(90, 90, 90))
    cell = s / 5
    for row in range(3):
        for col in range(5):
            wave = math.sin(col * 0.9) * s * 0.06
            fill = DARK if (row + col) % 2 == 0 else (255, 255, 255)
            d.rectangle([x + col * cell, y + row * cell + wave, x + (col + 1) * cell, y + (row + 1) * cell + wave],
                        fill=fill)


def gradient(w, h, top, bottom, checker=0):
    g = Image.new("RGB", (w, h))
    d = ImageDraw.Draw(g)
    for y in range(h):
        t = y / max(1, h - 1)
        d.line([(0, y), (w, y)], fill=tuple(int(top[i] + (bottom[i] - top[i]) * t) for i in range(3)))
    if checker:
        for y in range(0, h // 2, checker):
            for x in range(0, w, checker):
                if (x // checker + y // checker) % 2 == 0:
                    d.rectangle([x, y, x + checker - 1, y + checker - 1], fill=(255, 255, 255))
    return g


def letter(ch, f, fill, sw):
    """One glyph as RGBA: dark extruded outline, yellow rim, coloured face."""
    l, t, r, b = f.getbbox(ch, stroke_width=sw + 4)
    w, h = r - l + 12, b - t + 16
    out = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    def mask(stroke, dx=0, dy=0):
        m = Image.new("L", (w, h), 0)
        ImageDraw.Draw(m).text((6 - l + dx, 4 - t + dy), ch, font=f, fill=255,
                               stroke_width=stroke, stroke_fill=255)
        return m
    for dy in range(sw // 2 + 4, 0, -1):            # 3D depth
        out.paste((60, 25, 10, 255), (0, 0), mask(sw + 3, 0, dy))
    out.paste((40, 20, 10, 255), (0, 0), mask(sw + 3))
    out.paste(YELLOW + (255,), (0, 0), mask(sw))
    face = fill(w, h) if callable(fill) else Image.new("RGB", (w, h), fill)
    out.paste(face, (0, 0), mask(0))
    return out


def arc_line(img, text, cx, cy, size, bend, fill, max_w):
    """Letters on a gentle arc (bend = radius in px), shrunk to fit max_w."""
    while True:
        sw = max(4, size // 9)
        f = font(size)
        sprites = [letter(ch, f, fill, sw) if ch != " " else None for ch in text]
        widths = [s.width - 2 * sw if s else size * 0.3 for s in sprites]
        total = sum(widths) + 2 * sw
        if total <= max_w or size < 12:
            break
        size = int(size * 0.95)
    x = -total / 2
    for sp, wd in zip(sprites, widths):
        mid = x + wd / 2
        x += wd
        if sp is None:
            continue
        ang = mid / bend if bend else 0
        px = cx + (bend * math.sin(ang) if bend else mid)
        py = cy + (bend * (1 - math.cos(ang)) if bend else 0)
        rot = sp.rotate(-math.degrees(ang), resample=Image.BICUBIC, expand=True)
        img.paste(rot, (int(px - rot.width / 2), int(py - rot.height / 2)), rot)


def title(img, cx, cy, size, max_w):
    arc_line(img, "DIDDY KONG", cx, cy, size, size * 9, RED, max_w)
    cell = max(6, size // 8)
    racing = lambda w, h: gradient(w, h, (40, 200, 255), (150, 225, 40), checker=cell)
    arc_line(img, "RACING", cx, cy + int(size * 1.12), int(size * 0.95), size * 12, racing, max_w * 0.8)


SS = 2   # draw at twice the size, then downscale: smooth edges


def icon(out):
    s = 512 * SS
    img = sky(s, s)
    rays(img, s * 0.5, s * 0.34)
    hills(img, s * 0.70)
    trees(img, [s * 0.1, s * 0.9], s * 0.76, s * 0.18)
    balloon(img, int(s * 0.5), int(s * 0.27), int(s * 0.15))
    title(img, s // 2, int(s * 0.62), 80 * SS, s * 0.92)
    img.resize((512, 512), Image.LANCZOS).save(f"{out}/icon0.png", optimize=True)


def launch(out):
    w, h = 1920 * SS, 1080 * SS
    img = sky(w, h)
    rays(img, w * 0.5, h * 0.40, 24)
    hills(img, h * 0.72)
    trees(img, [w * 0.07, w * 0.16, w * 0.84, w * 0.93], h * 0.78, h * 0.2)
    flag(img, int(w * 0.72), int(h * 0.36), h * 0.15, h * 0.73)
    flag(img, int(w * 0.235), int(h * 0.36), h * 0.15, h * 0.73)
    balloon(img, w // 2, int(h * 0.27), int(h * 0.15))
    title(img, w // 2, int(h * 0.62), 130 * SS, w * 0.6)
    img.resize((1920, 1080), Image.LANCZOS).save(f"{out}/pic1.png", optimize=True)


if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else "."
    icon(out)
    launch(out)
