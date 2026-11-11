"""Compares the left-eye clock captures (SHADPS4_SHOT_SECONDS) of no-input runs at different game
frame rates: the k-th capture of every run is taken at the same time after start.

usage: idlecompare.py <out.png> <label>=<run dir> ...   (the first run is the reference)
Prints, per capture, the share of pixels differing (> 24 in any channel) from the reference run."""
import re
import sys

import numpy as np
from PIL import Image, ImageDraw

out_png = sys.argv[1]
runs = [a.split('=', 1) for a in sys.argv[2:]]
caps = {}
for label, run in runs:
    lst = []
    for line in open(run + r'\shad_log.txt', encoding='utf-8', errors='replace'):
        m = re.search(r'Saved screenshot: (.*_game_\d+_eye_0\.png)\s*$', line)
        if m:
            lst.append(m.group(1))
    caps[label] = lst
    print(f'{label}: {len(lst)} captures')

n = min(len(v) for v in caps.values())


def load(p):
    return np.asarray(Image.open(p).convert('RGB'), dtype=np.int16)


ref = runs[0][0]
print('capture  time   ' + '  '.join(f'{lab:>8}' for lab, _ in runs[1:]) + '   (% pixels differing from ' + ref + ')')
for k in range(n):
    a = load(caps[ref][k])
    row = []
    for lab, _ in runs[1:]:
        b = load(caps[lab][k])
        row.append(100.0 * (np.abs(a - b).max(axis=2) > 24).mean())
    print(f'{k:>7}  {50 + 2 * k:>3} s  ' + '  '.join(f'{v:8.1f}' for v in row))

cols = list(range(0, n, max(1, n // 8)))[:8]
tw = 240
w0, h0 = Image.open(caps[ref][0]).size
th = int(h0 * tw / w0)
sheet = Image.new('RGB', (60 + len(cols) * tw, len(runs) * (th + 4) + 18), (20, 20, 20))
d = ImageDraw.Draw(sheet)
for j, k in enumerate(cols):
    d.text((60 + j * tw + 4, 3), f'{50 + 2 * k} s', fill=(230, 230, 230))
for i, (lab, _) in enumerate(runs):
    y = 18 + i * (th + 4)
    d.text((4, y + th // 2), lab, fill=(230, 230, 230))
    for j, k in enumerate(cols):
        img = Image.open(caps[lab][k]).convert('RGB')
        img = Image.fromarray(np.asarray(img)[:, :, ::-1])  # captures are BGR
        sheet.paste(img.resize((tw, th)), (60 + j * tw, y))
sheet.save(out_png)
print('wrote', out_png)
