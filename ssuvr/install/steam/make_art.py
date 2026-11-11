"""Steam library art for Super Stardust Ultra VR (PC VR port), from PS Store EP9000-CUSA05951_00-SUPRSTARDULTVREU
assets (store page images saved in steam\\art-src, 2026-11-07). Same recipe as C:\\Games\\ShadowOfTheColossus\\steam\\make_art.py.
Usage: python -I make_art.py <art-src folder> <out folder>"""
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageFilter

SRC, OUT = Path(sys.argv[1]), Path(sys.argv[2])
OUT.mkdir(parents=True, exist_ok=True)
KEY = Image.open(SRC / 'BsCgJnYVixpg6jBcD613a0Vl.png').convert('RGB')    # 3840x2160 key art (GAMEHUB_COVER_ART), no logo
LOGO = Image.open(SRC / 'oUNjNxyYgsbpg5SktEB15oa8.png').convert('RGBA')  # 1531x592 wordmark, transparent (LOGO)
LOGO = LOGO.crop(LOGO.getbbox())
ICON = Image.open(SRC / 'UxVz30v82ZSVW93iB3gFVX1C8Q4ghqxh.png').convert('RGB')  # 1024 square, key art + logo (MASTER)
PLANET = (0.497, 0.47)  # burning planet's centre in KEY (fractions)


def place(img, size, scale, src, dst):
    """img scaled by `scale` (output px per source px), positioned so source point `src` (fractions) lands on `dst`
    (fractions of the output), clamped to stay inside the image."""
    tw, th = size
    im = img.resize((round(img.width * scale), round(img.height * scale)), Image.LANCZOS)
    x = int(min(max(im.width * src[0] - tw * dst[0], 0), im.width - tw))
    y = int(min(max(im.height * src[1] - th * dst[1], 0), im.height - th))
    return im.crop((x, y, x + tw, y + th))


def fit_width(im, w):
    return im.resize((w, round(im.height * w / im.width)), Image.LANCZOS)


def shadowed(logo, radius):
    pad = radius * 3
    out = Image.new('RGBA', (logo.width + 2 * pad, logo.height + 2 * pad), (0, 0, 0, 0))
    sh = Image.new('RGBA', logo.size, (0, 0, 0, 255))
    sh.putalpha(logo.getchannel('A').point(lambda v: int(v * 0.9)))
    out.alpha_composite(sh, (pad, pad))
    out = out.filter(ImageFilter.GaussianBlur(radius))
    out.alpha_composite(logo, (pad, pad))
    return out


def shade(size, axis, start, end, strength):
    """Black overlay rising from 0 at `start` to `strength` at `end` (fractions along x or y)."""
    w, h = size
    n = w if axis == 'x' else h
    t = np.linspace(0, 1, n)
    a = np.clip((t - start) / (end - start), 0, 1) ** 1.3 * strength
    a = a.astype(np.uint8)
    arr = a[None, :].repeat(h, 0) if axis == 'x' else a[:, None].repeat(w, 1)
    layer = Image.new('RGBA', size, (0, 0, 0, 0))
    layer.putalpha(Image.fromarray(arr))
    return layer


# Portrait 600x900: planet in the upper middle, logo over a darkened bottom.
p = place(KEY, (600, 900), 900 / 2160, PLANET, (0.5, 0.37)).convert('RGBA')  # smallest scale that fills
p.alpha_composite(shade(p.size, 'y', 0.62, 1.0, 200))
lg = shadowed(fit_width(LOGO, 520), 6)
p.alpha_composite(lg, ((600 - lg.width) // 2, 900 - lg.height - 22))
p.convert('RGB').save(OUT / 'portrait.png')

# Wide capsule 920x430: planet on the right, logo over a darkened left.
w = place(KEY, (920, 430), 0.26, PLANET, (0.72, 0.5)).convert('RGBA')
w.alpha_composite(shade(w.size, 'x', 0.6, 0.0, 170))
lg = shadowed(fit_width(LOGO, 400), 5)
w.alpha_composite(lg, (22, (430 - lg.height) // 2))
w.convert('RGB').save(OUT / 'wide.png')

# Hero 1920x620 (no logo; Steam draws logo.png over it, bottom left).
place(KEY, (1920, 620), 0.5, PLANET, (0.5, 0.5)).save(OUT / 'hero.png')  # smallest scale that fills

fit_width(LOGO, 1280).save(OUT / 'logo.png')
ic = ICON.resize((256, 256), Image.LANCZOS)
ic.save(OUT / 'icon.png')
ic.save(OUT / 'ssuvr.ico', sizes=[(256, 256), (64, 64), (48, 48), (32, 32), (16, 16)])
print('done', OUT)
