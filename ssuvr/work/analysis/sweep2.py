"""Reads a SHADPS4_DEBUG_SWEEP=<s>,<start>,alt[,right] run: what each pipeline adds to the picture.

usage: sweep2.py <shad_log.txt> <out dir> pause|right

pause: a still (paused) frame, each pipeline dropped in both eyes. A pixel is the dropped
       pipeline's own when it differs from every nothing-dropped capture (so blinking or turning
       menu parts, which the nothing-dropped captures catch in their states, do not count).
right: gameplay, each pipeline dropped in the right eye only. The eyes of each capture are compared
       allowing for parallax (a left-eye pixel counts when no right-eye pixel within +-REACH px on
       its row is close to it); the nothing-dropped steps give the level of that noise.
exact: as right, for runs with "ipd_mm": 0 in vr.json (both eyes see the same): left and right
       are compared pixel for pixel; pixels that differ in most nothing-dropped captures are left
       out; a step's captures are merged (the one with most changed pixels counts).
Writes <out>/sweep2.csv and <out>/sweep2-sheet.png (left eye, the pipeline's pixels in magenta)."""
import csv
import os
import re
import sys

import numpy as np
from PIL import Image, ImageDraw

log_path, out_dir, mode = sys.argv[1:4]
os.makedirs(out_dir, exist_ok=True)
THR = 24
REACH = 32

steps, pairs = [], []
for line in open(log_path, encoding='utf-8', errors='replace'):
    m = re.search(r'SWEEP step (\d+)/(\d+): (.*)$', line)
    if m:
        steps.append((int(m.group(1)), int(m.group(2)), m.group(3).strip()))
        pairs.append({})
        continue
    m = re.search(r'Saved screenshot: (.*_eye_([01])\.png)\s*$', line)
    if m and pairs:
        pairs[-1].setdefault(int(m.group(2)), m.group(1))
undrawn = [l.strip() for l in open(log_path, encoding='utf-8', errors='replace') if 'did not draw' in l or 'SWEEP skips' in l or 'SWEEP learned' in l]
for l in undrawn:
    print(l[l.index('SWEEP'):][:160])
keep = [i for i in range(len(steps)) if len(pairs[i]) == 2]
steps = [steps[i] for i in keep]
pairs = [pairs[i] for i in keep]
if steps:
    print(f'{len(steps)} captured steps of {steps[0][1]}')


def load(path):
    return np.asarray(Image.open(path).convert('RGB'), dtype=np.int16)


def unmatched(a, b, reach):
    """Pixels of a with no pixel of b within +-reach on the same row closer than THR."""
    h, w, _ = a.shape
    best = np.full((h, w), 1 << 14, dtype=np.int16)
    for dx in range(-reach, reach + 1):
        shifted = np.full_like(b, -4096)
        if dx >= 0:
            shifted[:, :w - dx] = b[:, dx:]
        else:
            shifted[:, -dx:] = b[:, :w + dx]
        np.minimum(best, np.abs(a - shifted).max(axis=2), out=best)
    return best > THR


nothing = [i for i, s in enumerate(steps) if s[2].startswith('nothing')]
rows = []
if mode == 'pause':
    bases = [(load(pairs[i][0]), load(pairs[i][1])) for i in nothing]
    for i, (n, last, what) in enumerate(steps):
        if i in nothing:
            continue
        own = []
        for e in (0, 1):
            img = load(pairs[i][e])
            diff = np.min([np.abs(img - b[e]).max(axis=2) for b in bases], axis=0)
            own.append(diff > THR)
        rows.append((n, what, int(own[0].sum()), int(own[1].sum()), own[0] | own[1], i))
    # Nothing-dropped captures against each other: how still the frame is.
    spread = []
    for k in range(1, len(bases)):
        for e in (0, 1):
            spread.append(int((np.abs(bases[k][e] - bases[0][e]).max(axis=2) > THR).sum()))
    print(f'{len(bases)} nothing-dropped captures; pixels differing from the first: '
          f'max {max(spread) if spread else 0}, median {int(np.median(spread)) if spread else 0}')
elif mode == 'exact':
    diffs = []
    for i in range(len(steps)):
        left, right = load(pairs[i][0]), load(pairs[i][1])
        diffs.append(np.abs(left - right).max(axis=2) > THR)
    base = [d for d, s in zip(diffs, steps) if s[2].startswith('nothing')]
    always = (np.mean(base, axis=0) >= 0.5) if base else np.zeros_like(diffs[0])
    counts = [int((d & ~always).sum()) for d, s in zip(diffs, steps) if s[2].startswith('nothing')]
    print(f'{len(base)} nothing-dropped captures: {int(always.sum())} px differ between the eyes in most '
          f'(left out); the rest: max {max(counts) if counts else 0}, median {int(np.median(counts)) if counts else 0}')
    best = {}
    for i, (n, last, what) in enumerate(steps):
        if what.startswith('nothing'):
            continue
        own = diffs[i] & ~always
        c = int(own.sum())
        if n not in best or c > best[n][2]:
            best[n] = (n, re.sub(r' \| capture \d+,', ' |', what), c, 0, own, i)
    rows = [best[n] for n in sorted(best)]
    floor = max(counts) if counts else 0
    rows = [r + (floor,) for r in rows]
else:
    noise = {}
    for i, (n, last, what) in enumerate(steps):
        left, right = load(pairs[i][0]), load(pairs[i][1])
        lost = unmatched(left, right, REACH)   # in the left eye, missing from the right
        count = int(lost.sum())
        if i in nothing:
            noise[i] = count
            continue
        rows.append((n, what, count, int(unmatched(right, left, REACH).sum()), lost, i))
    levels = sorted(noise.values())
    print(f'nothing-dropped steps: left-eye pixels with no match in the right eye: '
          f'min {levels[0]}, median {levels[len(levels) // 2]}, max {levels[-1]}')
    # Each drop step against its neighbours (the nothing-dropped steps around it).
    for k, row in enumerate(rows):
        i = row[5]
        around = [noise[j] for j in (i - 1, i + 1) if j in noise]
        rows[k] = row + (max(around) if around else levels[-1],)

with open(os.path.join(out_dir, 'sweep2.csv'), 'w', newline='', encoding='utf-8') as f:
    w = csv.writer(f)
    w.writerow(['step', 'what', 'px_a', 'px_b', 'noise'])
    for row in rows:
        w.writerow([row[0], row[1], row[2], row[3], row[6] if len(row) > 6 else ''])

for row in rows:
    n, what, a, b = row[:4]
    if mode == 'pause':
        flag = '   <-- NOTHING OF ITS OWN' if a + b < 20 else ''
        print(f'{n:>3} {a:>7} {b:>7}  {what[:110]}{flag}')
    elif mode == 'exact':
        flag = '   <-- NO CHANGE' if a <= row[6] else ''
        print(f'{n:>3} {a:>7}  {what[:120]}{flag}')
    else:
        extra = a - row[6]
        flag = '   <-- within noise' if extra < 50 else ''
        print(f'{n:>3} {a:>7} (neighbours {row[6]:>6}, +{extra:>6})  {what[:100]}{flag}')

# Contact sheet.
tw = 240
h0, w0 = Image.open(pairs[0][0]).size[1], Image.open(pairs[0][0]).size[0]
th = int(h0 * tw / w0)
cols = 8
lines = max(1, (len(rows) + cols - 1) // cols)
sheet = Image.new('RGB', (cols * tw, lines * (th + 26)), (20, 20, 20))
draw = ImageDraw.Draw(sheet)
for k, row in enumerate(rows):
    n, what, a, b, mask, i = row[:6]
    img = np.asarray(Image.open(pairs[i][0]).convert('RGB')).copy()
    grown = mask.copy()
    for d in (-2, -1, 1, 2):
        grown |= np.roll(mask, d, axis=0) | np.roll(mask, d, axis=1)
    img[grown] = (img[grown] // 3) + np.array([170, 0, 170], dtype=np.uint8)
    thumb = Image.fromarray(img).resize((tw, th))
    x, y = (k % cols) * tw, (k // cols) * (th + 26)
    sheet.paste(thumb, (x, y))
    m = re.search(r'ps ([0-9a-f]{16})', what)
    draw.text((x + 3, y + th + 3), f'{n}: {m.group(1)[-8:] if m else "?"} {a}', fill=(230, 230, 230))
sheet.save(os.path.join(out_dir, 'sweep2-sheet.png'))
print('wrote', os.path.join(out_dir, 'sweep2-sheet.png'))
