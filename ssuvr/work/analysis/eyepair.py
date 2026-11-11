"""Left and right eye captures of one frame side by side (R/B swapped back), same crop, to see what a
right-eye-only SHADPS4_DEBUG_SKIP_RIGHT run removed. The right eye's crop is shifted left by
`shift` px to roughly undo the stereo parallax at the planet's depth.
usage: eyepair.py <eye_0 png> <out png> <x0> <y0> <x1> <y1> [scale 0.5] [shift 0]"""
import glob
import os
import re
import sys
from PIL import Image

left_path, out = sys.argv[1], sys.argv[2]
x0, y0, x1, y1 = (int(v) for v in sys.argv[3:7])
scale = float(sys.argv[7]) if len(sys.argv) > 7 else 0.5
shift = int(sys.argv[8]) if len(sys.argv) > 8 else 0
# The right eye is written right after the left one: same stamp prefix, next game index, _eye_1.
m = re.match(r'(.*_)(\d{6})_(\d{3})_game_(\d{6})_eye_0\.png$', left_path)
candidates = sorted(glob.glob(m.group(1) + m.group(2) + '_*_game_*_eye_1.png'))
right_path = candidates[0]


def load(path, box):
    im = Image.open(path).convert('RGB')
    r, g, b = im.split()
    im = Image.merge('RGB', (b, g, r)).crop(box)
    return im.resize((int(im.width * scale), int(im.height * scale)), Image.LANCZOS)


a = load(left_path, (x0, y0, x1, y1))
b = load(right_path, (x0 - shift, y0, x1 - shift, y1))
sheet = Image.new('RGB', (a.width * 2 + 8, a.height), (40, 40, 40))
sheet.paste(a, (0, 0))
sheet.paste(b, (a.width + 8, 0))
sheet.save(out)
print(out, sheet.size, os.path.basename(right_path))
