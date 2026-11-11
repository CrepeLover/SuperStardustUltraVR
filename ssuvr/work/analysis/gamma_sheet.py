"""Compare a raw eye capture (the game's eye texture bytes) with what the headset path makes of it.

The headset path (post_process.frag, local target: linear_out = 1, srgb_input = 0, R8G8B8A8Srgb target)
treats the eye texture's UNORM bytes x as linear light: the headset finally shows light = x.
- If the game's bytes are gamma-encoded (as for a TV), the right light is decode(x); the raw PNG
  then shows the intended picture and the headset shows encode(x) on an sRGB screen: brighter.
- If the game's bytes are linear, the raw PNG looks too dark and the headset is right.
Builds: [PS4 reference] [raw capture = intended if encoded] [what the headset shows today, as sRGB].
usage: gamma_sheet.py <raw eye png> <ps4 reference> <out png> [crop x0 y0 x1 y1 of the raw]"""
import sys
import numpy as np
from PIL import Image

raw_path, ref_path, out_path = sys.argv[1:4]
crop = tuple(int(v) for v in sys.argv[4:8]) if len(sys.argv) >= 8 else None


def encode(lin):
    lin = np.clip(lin, 0.0, 1.0)
    return np.where(lin <= 0.0031308, lin * 12.92, 1.055 * np.power(lin, 1 / 2.4) - 0.055)


raw = Image.open(raw_path).convert('RGB')
# The emulator's eye captures copy the image bytes without the texture's swizzle: red and blue
# come out swapped (the grid sphere is cyan in SteamVR's own capture and on PS4, yellow raw).
r, g, b = raw.split()
raw = Image.merge('RGB', (b, g, r))
if crop:
    raw = raw.crop(crop)
H = 720
raw = raw.resize((int(raw.width * H / raw.height), H), Image.LANCZOS)
x = np.asarray(raw).astype(np.float32) / 255.0
headset = Image.fromarray((encode(x) * 255.0 + 0.5).astype(np.uint8))
ref = Image.open(ref_path).convert('RGB')
ref = ref.resize((int(ref.width * H / ref.height), H), Image.LANCZOS)
from PIL import ImageDraw, ImageFont
TOP = 44
sheet = Image.new('RGB', (ref.width + raw.width * 2 + 40, H + TOP), (40, 40, 40))
sheet.paste(ref, (0, TOP))
sheet.paste(raw, (ref.width + 20, TOP))
sheet.paste(headset, (ref.width + raw.width + 40, TOP))
draw = ImageDraw.Draw(sheet)
font = ImageFont.load_default(size=26)
for left, text in ((8, 'PS4 (same level, TV)'), (ref.width + 28, 'Headset now: decoded once (fixed)'),
                   (ref.width + raw.width + 48, 'Headset before: encoded twice')):
    draw.text((left, 8), text, fill=(255, 255, 255), font=font)
sheet.save(out_path)


def stats(name, a):
    lum = (0.2126 * a[..., 0] + 0.7152 * a[..., 1] + 0.0722 * a[..., 2])
    print('%-28s mean %.3f  median %.3f  p10 %.3f  p90 %.3f  dark(<0.1) %.0f%%' % (
        name, lum.mean(), np.median(lum), np.percentile(lum, 10), np.percentile(lum, 90), 100 * (lum < 0.1).mean()))


stats('PS4 reference', np.asarray(ref).astype(np.float32) / 255)
stats('raw eye bytes', x)
stats('headset today (as sRGB)', encode(x))
print('wrote', out_path, sheet.size)
