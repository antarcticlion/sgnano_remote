#!/usr/bin/env python3
# sgnano_remote のアイコン（2026-11-15）。角の丸い黄色い五芒星に、目・ほお・笑顔を足したポップな絵。
# 生成物: resources/icons/sgnano_remote.png / sgnano_remote.ico
from PIL import Image, ImageDraw, ImageFilter
import math
import os

S = 1024
OUT = os.path.join(os.path.dirname(__file__), "..", "..", "resources", "icons")
os.makedirs(OUT, exist_ok=True)


def star_points(cx, cy, outer, inner):
    pts = []
    for i in range(10):
        a = math.radians(-90 + i * 36)
        r = outer if i % 2 == 0 else inner
        pts.append((cx + math.cos(a) * r, cy + math.sin(a) * r))
    return pts


points = star_points(S / 2, 540, 420, 205)
outline = (196, 122, 0, 255)
ink = (91, 56, 25, 255)

# 透明な所の RGB も黄色にしておく（小さい大きさへ縮めたときの黒いふちを避ける）
img = Image.new("RGBA", (S, S), (196, 122, 0, 0))


def rounded_shape(draw, width, fill):
    """線の継ぎ目に隙間ができないよう、線分と頂点の丸を別々に描く。"""
    draw.polygon(points, fill=fill)
    draw.line(points + [points[0]], fill=fill, width=width)
    r = width / 2
    for x, y in points:
        draw.ellipse((x - r, y - r, x + r, y + r), fill=fill)


# 影（下へ少しずらしてぼかす）
shadow = Image.new("L", (S, S), 0)
d = ImageDraw.Draw(shadow)
rounded_shape(d, 84, 150)
shadow = shadow.filter(ImageFilter.GaussianBlur(22))
shadow_layer = Image.new("RGBA", (S, S), (214, 155, 0, 0))
shadow_layer.putalpha(shadow)
img.alpha_composite(shadow_layer, (0, 26))

# 太い輪郭（先を丸くする）
d = ImageDraw.Draw(img)
rounded_shape(d, 88, outline)

# 黄色の本体。上から下へのグラデーション
fill_mask = Image.new("L", (S, S), 0)
fm = ImageDraw.Draw(fill_mask)
rounded_shape(fm, 58, 255)

gradient = Image.new("RGBA", (S, S))
gd = ImageDraw.Draw(gradient)
for y in range(S):
    t = y / (S - 1)
    r = int(255 - 8 * t)
    g = int(229 - 43 * t)
    b = int(80 - 30 * t)
    gd.line([(0, y), (S, y)], fill=(r, g, b, 255))
img.paste(gradient, (0, 0), fill_mask)

# つや（左上の柔らかい白）
gloss = Image.new("L", (S, S), 0)
gl = ImageDraw.Draw(gloss)
gl.ellipse((230, 170, 760, 520), fill=80)
gloss = gloss.filter(ImageFilter.GaussianBlur(48))
gloss_layer = Image.new("RGBA", (S, S), (255, 255, 255, 0))
gloss_layer.putalpha(gloss)
gloss_layer.putalpha(Image.composite(gloss, Image.new("L", (S, S), 0), fill_mask))
img.alpha_composite(gloss_layer)

# ほお
cheek = Image.new("RGBA", (S, S), (0, 0, 0, 0))
cd = ImageDraw.Draw(cheek)
cd.ellipse((352, 610, 448, 666), fill=(255, 125, 145, 135))
cd.ellipse((576, 610, 672, 666), fill=(255, 125, 145, 135))
img.alpha_composite(cheek)

# 目
d = ImageDraw.Draw(img)
for x in (438, 586):
    d.ellipse((x - 24, 536, x + 24, 584), fill=ink)
    d.ellipse((x - 8, 546, x + 4, 558), fill=(255, 255, 255, 235))

# 笑顔
d.arc((466, 566, 558, 640), 20, 160, fill=ink, width=12)

# 小さなきらきら
def sparkle(cx, cy, r, color):
    p = [(cx, cy - r), (cx + r * 0.28, cy - r * 0.28), (cx + r, cy),
         (cx + r * 0.28, cy + r * 0.28), (cx, cy + r),
         (cx - r * 0.28, cy + r * 0.28), (cx - r, cy),
         (cx - r * 0.28, cy - r * 0.28)]
    d.polygon(p, fill=color)

sparkle(178, 330, 30, (255, 255, 255, 220))
sparkle(820, 700, 24, (255, 255, 255, 200))
sparkle(780, 260, 16, (255, 255, 255, 180))

def resize_icon(image, size):
    """透明な所も輪郭と同じ色にしておき、縮めた縁に黒や色の点が出ないようにする。"""
    return image.resize((size, size), Image.LANCZOS)


# PNG（アプリ内のアイコンと Linux 用）
for size in (512, 256, 128, 64, 48, 32, 16):
    resize_icon(img, size).save(os.path.join(OUT, f"sgnano_remote_{size}.png"))

# Windows の .ico（複数の大きさ）
ico = resize_icon(img, 256)
ico.save(os.path.join(OUT, "sgnano_remote.ico"), format="ICO",
         sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])

print("wrote", OUT)
