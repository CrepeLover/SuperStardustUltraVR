"""Contact sheet of raw emulator eye captures (R/B swapped back), each scaled to a common height, with an
optional crop box (as fractions of the image) to zoom on the middle where the ship sits.
usage: shotsheet.py <out png> <height> <crop x0 y0 x1 y1 fractions or 'full'> <png>...  """
import sys
from PIL import Image, ImageDraw, ImageFont

out, height, crop = sys.argv[1], int(sys.argv[2]), sys.argv[3]
paths = sys.argv[4:]
font = ImageFont.load_default(size=20)
tiles = []
for p in paths:
    try:
        im = Image.open(p).convert('RGB')
    except Exception as e:  # a capture cut short when the run was stopped
        print('skipped', p, type(e).__name__)
        continue
    r, g, b = im.split()
    im = Image.merge('RGB', (b, g, r))
    if crop != 'full':
        fx0, fy0, fx1, fy1 = (float(v) for v in crop.split(','))
        im = im.crop((int(fx0 * im.width), int(fy0 * im.height), int(fx1 * im.width), int(fy1 * im.height)))
    im = im.resize((int(im.width * height / im.height), height), Image.LANCZOS)
    d = ImageDraw.Draw(im)
    label = p.replace('\\', '/').split('/')[-1].replace('CUSA05951_', '')[:40]
    d.rectangle((0, 0, 9 * len(label) + 12, 26), fill=(0, 0, 0))
    d.text((6, 2), label, fill=(255, 255, 0), font=font)
    tiles.append(im)
cols = min(3, len(tiles))
rows = (len(tiles) + cols - 1) // cols
w = max(t.width for t in tiles)
sheet = Image.new('RGB', (cols * w + (cols - 1) * 8, rows * height + (rows - 1) * 8), (40, 40, 40))
for i, t in enumerate(tiles):
    sheet.paste(t, ((i % cols) * (w + 8), (i // cols) * (height + 8)))
sheet.save(out)
print(out, sheet.size)
