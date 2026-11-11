"""View a raw emulator eye capture as the headset shows it now (R/B swapped back; the bytes are
already display-encoded). usage: eyeview.py <eye png> <out png> [scale 0.25] [crop x0 y0 x1 y1 in source px]"""
import sys
from PIL import Image

src, out = sys.argv[1], sys.argv[2]
scale = float(sys.argv[3]) if len(sys.argv) > 3 else 0.25
im = Image.open(src).convert('RGB')
r, g, b = im.split()
im = Image.merge('RGB', (b, g, r))
if len(sys.argv) >= 8:
    im = im.crop(tuple(int(v) for v in sys.argv[4:8]))
if scale != 1.0:
    im = im.resize((max(1, int(im.width * scale)), max(1, int(im.height * scale))), Image.LANCZOS)
im.save(out)
print(out, im.size)
