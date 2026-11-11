"""Reads a SHADPS4_DEBUG_SWEEP run: which pipelines change the picture when dropped.

usage: sweepdiff.py <shad_log.txt> <screenshots dir> <since ISO time> <out dir> [pause|right]

pause: the frame holds still (paused game) and every pipeline was dropped in both eyes; each
       step's eye pictures are compared with the nothing-dropped steps (first and last).
right: the pipeline was dropped in the right eye only; the eyes of one capture are compared,
       allowing for parallax (a left-eye pixel counts as changed when no right-eye pixel within
       +-D px on its row is close to it).
Writes <out>/sweep.csv and <out>/sweep-sheet.png (left eye, changed pixels in magenta)."""
import csv
import datetime as dt
import os
import re
import sys

import numpy as np
from PIL import Image, ImageDraw

log_path, shots_dir, since, out_dir = sys.argv[1:5]
mode = sys.argv[5] if len(sys.argv) > 5 else 'pause'
os.makedirs(out_dir, exist_ok=True)
since_ts = dt.datetime.fromisoformat(since.strip().lstrip('﻿')).timestamp()

steps = []
pairs = []
for line in open(log_path, encoding='utf-8', errors='replace'):
    m = re.search(r'SWEEP step (\d+)/(\d+): (.*)$', line)
    if m:
        steps.append((int(m.group(1)), m.group(3).strip()))
        pairs.append({})
        continue
    m = re.search(r'Saved screenshot: (.*_eye_([01])\.png)\s*$', line)
    if m and pairs:
        pairs[-1].setdefault(int(m.group(2)), m.group(1))
keep = [i for i in range(len(steps)) if len(pairs[i]) == 2]
steps = [steps[i] for i in keep]
pairs = [pairs[i] for i in keep]
print(f'{len(steps)} logged steps, {len(pairs)} eye captures')
if len(pairs) != len(steps):
    print('WARNING: capture count differs from step count; matching by order from the start')
count = min(len(pairs), len(steps))


def load(path):
    return np.asarray(Image.open(path).convert('RGB'), dtype=np.int16)


def changed_same(a, b, thr):
    return np.abs(a - b).max(axis=2) > thr


def changed_parallax(left, right, thr, reach):
    """Left pixels with no right pixel within +-reach on the same row closer than thr."""
    h, w, _ = left.shape
    best = np.full((h, w), 1 << 14, dtype=np.int16)
    for dx in range(-reach, reach + 1):
        shifted = np.full_like(right, -4096)
        if dx >= 0:
            shifted[:, :w - dx] = right[:, dx:]
        else:
            shifted[:, -dx:] = right[:, :w + dx]
        best = np.minimum(best, np.abs(left - shifted).max(axis=2))
    return best > thr


THR = 24
rows = []
images = [(load(pairs[i][0]), load(pairs[i][1])) for i in range(count)]
if mode == 'pause':
    base_idx = [i for i in range(count) if steps[i][1].startswith('nothing')]
    b0 = images[base_idx[0]]
    noise = [int(changed_same(images[j][e], b0[e], THR).sum()) for j in base_idx[1:] for e in (0, 1)]
    print(f'still-frame check: changed pixels between nothing-dropped captures {noise}')
    for i in range(count):
        cl = changed_same(images[i][0], b0[0], THR)
        cr = changed_same(images[i][1], b0[1], THR)
        rows.append((steps[i], int(cl.sum()), int(cr.sum()), cl))
else:
    reach = 24
    for i in range(count):
        left, right = images[i]
        lr = changed_parallax(left, right, THR, reach)
        rl = changed_parallax(right, left, THR, reach)
        rows.append((steps[i], int(lr.sum()), int(rl.sum()), lr | rl[:, :]))

with open(os.path.join(out_dir, 'sweep.csv'), 'w', newline='', encoding='utf-8') as f:
    w = csv.writer(f)
    w.writerow(['step', 'what', 'changed_px_a', 'changed_px_b'])
    for (n, what), a, b, _ in rows:
        w.writerow([n, what, a, b])

for (n, what), a, b, _ in rows:
    print(f'{n:>3} {a:>8} {b:>8}  {what[:150]}')

# Contact sheet: left eye thumbnails with the changed pixels in magenta.
tw = 320
h0, w0, _ = images[0][0].shape
th = int(h0 * tw / w0)
cols = 6
lines = (len(rows) + cols - 1) // cols
sheet = Image.new('RGB', (cols * tw, lines * (th + 28)), (20, 20, 20))
draw = ImageDraw.Draw(sheet)
for k, ((n, what), a, b, mask) in enumerate(rows):
    img = images[k][0].astype(np.uint8).copy()
    grown = mask.copy()
    for dy in (-2, -1, 1, 2):
        grown |= np.roll(mask, dy, axis=0)
    for dx in (-2, -1, 1, 2):
        grown |= np.roll(mask, dx, axis=1)
    img[grown] = (img[grown] // 3) + np.array([170, 0, 170], dtype=np.uint8)
    thumb = Image.fromarray(img).resize((tw, th))
    x, y = (k % cols) * tw, (k // cols) * (th + 28)
    sheet.paste(thumb, (x, y))
    m = re.search(r'ps ([0-9a-f]{16})', what)
    label = f'{n}: ' + (m.group(1)[-8:] if m else what[:16]) + f'  {a}/{b}'
    draw.text((x + 4, y + th + 4), label, fill=(230, 230, 230))
sheet.save(os.path.join(out_dir, 'sweep-sheet.png'))
print('wrote', os.path.join(out_dir, 'sweep-sheet.png'))
