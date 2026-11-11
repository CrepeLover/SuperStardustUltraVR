"""Reads a SHADPS4_DEBUG_SWEEP=...,alt,freeze run (rounds of still gameplay frames).

usage: sweep3.py <shad_log.txt> <out dir>

Per round, a pixel is the dropped pipeline's own when it differs (any channel > THR) from every
nothing-dropped capture of that round, in either eye; a step's two captures are merged (the larger
count counts). Pipelines whose own pixels stay at the round's noise level are flagged, except the
ones the paused sweep already explained (KNOWN). Writes <out>/sweep3.csv and
<out>/round<r>-sheet.png (left eye, own pixels in magenta)."""
import csv
import os
import re
import sys

import numpy as np
from PIL import Image, ImageDraw

log_path, out_dir = sys.argv[1:3]
os.makedirs(out_dir, exist_ok=True)
THR = 24
KNOWN = {
    '00000ceb3555b1a9': 'depth-only clear',
    '000006fcf70e8ad2': 'left eye composite (blind spot)',
    '0000073cbeef7ae4': 'right eye composite (blind spot)',
    '000003c2634e42d3': 'coarse bloom blur (faint)',
    '00000dc24c92d778': 'coarse bloom blur (faint)',
    '00000784d2991f0b': 'coarse bloom add (faint, weight 0.5)',
}

caps = []  # (round, step, last, what, {eye: path})
for line in open(log_path, encoding='utf-8', errors='replace'):
    m = re.search(r'SWEEP round (\d+) step (\d+)/(\d+): (.*)$', line)
    if m and 'did not draw' not in line:
        caps.append([int(m.group(1)), int(m.group(2)), int(m.group(3)), m.group(4).strip(), {}])
        continue
    if 'Freeze:' in line:
        print(line[line.index('Freeze:'):].rstrip()[:160])
    elif 'SWEEP' in line and ('round' in line or 'freezes' in line or 'learned' in line or
                            'done' in line or 'did not draw' in line or 'run again' in line or
                            'not yet swept' in line or 'skips' in line or 'cannot' in line):
        print(line[line.index('SWEEP'):].rstrip()[:200])
    elif line.startswith('    ps ') or line.startswith('    vs '):
        print('   ', line.strip())
    m = re.search(r'Saved screenshot: (.*_eye_([01])\.png)\s*$', line)
    if m and caps:
        caps[-1][4].setdefault(int(m.group(2)), m.group(1))
caps = [c for c in caps if len(c[4]) == 2]
print(f'{len(caps)} captures with both eyes')


def load(path):
    return np.asarray(Image.open(path).convert('RGB'), dtype=np.int16)


rows = []
for r in sorted({c[0] for c in caps}):
    cs = [c for c in caps if c[0] == r]
    bases = [(load(c[4][0]), load(c[4][1])) for c in cs if c[3].startswith('nothing')]
    if not bases:
        print(f'round {r}: no nothing-dropped captures')
        continue
    # Noise: each nothing-dropped capture against the others.
    noise = []
    for j in range(len(bases)):
        others = [b for k, b in enumerate(bases) if k != j]
        if not others:
            break
        n = 0
        for e in (0, 1):
            d = np.min([np.abs(bases[j][e] - o[e]).max(axis=2) for o in others], axis=0)
            n += int((d > THR).sum())
        noise.append(n)
    floor = max(noise) if noise else 0
    print(f'round {r}: {len(bases)} nothing-dropped captures; own-pixel noise between them: '
          f'max {floor}, median {int(np.median(noise)) if noise else 0}')
    best = {}
    for c in cs:
        if c[3].startswith('nothing'):
            continue
        own_l = np.min([np.abs(load(c[4][0]) - b[0]).max(axis=2) for b in bases], axis=0) > THR
        own_r = np.min([np.abs(load(c[4][1]) - b[1]).max(axis=2) for b in bases], axis=0) > THR
        count = int(own_l.sum() + own_r.sum())
        what = re.sub(r' \| capture \d+,', ' |', c[3])
        if c[1] not in best or count > best[c[1]][3]:
            best[c[1]] = (r, c[1], what, count, own_l | own_r, c[4][0], floor)
    rows += [best[k] for k in sorted(best)]

with open(os.path.join(out_dir, 'sweep3.csv'), 'w', newline='', encoding='utf-8') as f:
    w = csv.writer(f)
    w.writerow(['round', 'step', 'what', 'own_px_both_eyes', 'round_noise'])
    for r, n, what, count, _, _, floor in rows:
        w.writerow([r, n, what, count, floor])

print()
for r, n, what, count, _, _, floor in rows:
    ps = re.search(r'ps ([0-9a-f]{16})', what)
    known = KNOWN.get(ps.group(1)) if ps else None
    flag = ''
    if count <= max(floor, 20):
        flag = f'   (no change: {known})' if known else '   <-- NO CHANGE OF ITS OWN'
    print(f'r{r} {n:>3} {count:>8} (noise {floor:>5})  {what[:105]}{flag}')

for r in sorted({row[0] for row in rows}):
    rr = [row for row in rows if row[0] == r]
    tw = 240
    w0, h0 = Image.open(rr[0][5]).size
    th = int(h0 * tw / w0)
    cols = 8
    sheet = Image.new('RGB', (cols * tw, ((len(rr) + cols - 1) // cols) * (th + 26)), (20, 20, 20))
    draw = ImageDraw.Draw(sheet)
    for k, (_, n, what, count, mask, path, _) in enumerate(rr):
        img = np.asarray(Image.open(path).convert('RGB')).copy()
        h = img.shape[0]
        m = mask[:, :img.shape[1]] if mask.shape == img.shape[:2] else mask
        grown = m.copy()
        for d in (-2, -1, 1, 2):
            grown |= np.roll(m, d, axis=0) | np.roll(m, d, axis=1)
        img[grown] = (img[grown] // 3) + np.array([170, 0, 170], dtype=np.uint8)
        x, y = (k % cols) * tw, (k // cols) * (th + 26)
        sheet.paste(Image.fromarray(img).resize((tw, th)), (x, y))
        ps = re.search(r'ps ([0-9a-f]{16})', what)
        draw.text((x + 3, y + th + 3), f'{n}: {ps.group(1)[-8:] if ps else "?"} {count}',
                  fill=(230, 230, 230))
    out = os.path.join(out_dir, f'round{r}-sheet.png')
    sheet.save(out)
    print('wrote', out)
