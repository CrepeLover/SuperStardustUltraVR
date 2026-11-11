"""Reads a SHADPS4_DEBUG_SWEEP=...,right,until=<s> run made with vr.json
{"ipd_mm": 0, "fov_tan": [1.2, 1.2, 1.262872, 1.262872]} (both eyes see the same image, to a few
pixels), during live play.

usage: sweep4.py <shad_log.txt> <out dir>

A dropped pipeline's own pixels are the ones where the right eye (dropped) differs from the left
eye (kept) of the same frame by more than THR in any channel. Noise is the same count on the
nothing-dropped captures. For each pipeline the capture with the most own pixels counts (over all
rounds). Pipelines with no more own pixels than the noise are flagged, except the ones the paused
sweep explained (KNOWN). Writes <out>/sweep4.csv and <out>/sheet<k>.png (left eye, the pipeline's
own pixels in magenta)."""
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
}

caps = []  # [round, step, last, what, {eye: path}]
did_not_draw = {}
for line in open(log_path, encoding='utf-8', errors='replace'):
    m = re.search(r'SWEEP round (\d+) step (\d+)/(\d+): (.*)$', line)
    if m:
        if 'did not draw' in line:
            ps = re.search(r'(ps [0-9a-f]{16} vs [0-9a-f]{16})', line).group(1)
            did_not_draw[ps] = did_not_draw.get(ps, 0) + 1
        else:
            caps.append([int(m.group(1)), int(m.group(2)), int(m.group(3)), m.group(4).strip(), {}])
        continue
    if 'SWEEP' in line and ('round' in line or 'learned' in line or 'done' in line or
                            'skips' in line or 'cannot' in line):
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


def own(c):
    a, b = load(c[4][0]), load(c[4][1])
    return np.abs(a - b).max(axis=2) > THR


noise = [int(own(c).sum()) for c in caps if c[3].startswith('nothing')]
floor = max(noise) if noise else 0
print(f'{len(noise)} nothing-dropped captures; left/right difference: max {floor}, '
      f'median {int(np.median(noise)) if noise else 0}')

best = {}  # pipeline -> (count, round, step, what, mask, left path, captures)
every = {}  # pipeline -> [(round, step, count, mask, left path)]
for c in caps:
    if c[3].startswith('nothing'):
        continue
    key = re.search(r'(ps [0-9a-f]{16} vs [0-9a-f]{16})', c[3]).group(1)
    mask = own(c)
    count = int(mask.sum())
    every.setdefault(key, []).append((c[0], c[1], count, mask, c[4][0]))
    prev = best.get(key)
    n = (prev[6] if prev else 0) + 1
    if prev is None or count > prev[0]:
        best[key] = (count, c[0], c[1], re.sub(r' \| capture \d+,', ' |', c[3]), mask, c[4][0], n)
    else:
        best[key] = prev[:6] + (n,)

rows = sorted(best.items(), key=lambda kv: -kv[1][0])
quiet = max(floor, 20)
with open(os.path.join(out_dir, 'sweep4.csv'), 'w', newline='', encoding='utf-8') as f:
    w = csv.writer(f)
    w.writerow(['pipeline', 'own_px_max', 'own_px_median', 'captures', 'silent_captures',
                'rounds', 'noise_max', 'what'])
    for key, (count, r, n, what, _, _, k) in rows:
        counts = [e[2] for e in every[key]]
        w.writerow([key, count, int(np.median(counts)), k, sum(x <= quiet for x in counts),
                    len({e[0] for e in every[key]}), floor, what])

print()
print(f'{"max px":>8} {"median":>7} {"caps":>4} {"silent":>6} {"rounds":>6}  pipeline')
for key, (count, r, n, what, _, _, k) in rows:
    ps = key.split()[1]
    counts = [e[2] for e in every[key]]
    flag = ''
    if count <= quiet:
        flag = f'   (no change: {KNOWN[ps]})' if ps in KNOWN else '   <-- NO CHANGE OF ITS OWN'
    draws = re.search(r'\| (.*)$', what)
    print(f'{count:>8} {int(np.median(counts)):>7} {k:>4} {sum(x <= quiet for x in counts):>6} '
          f'{len({e[0] for e in every[key]}):>6}  {key}  {draws.group(1) if draws else ""}{flag}')

# One strip per pipeline: every capture (left eye, own pixels in magenta), in time order.
sw = 200
for key, es in every.items():
    w0, h0 = Image.open(es[0][4]).size
    sh = int(h0 * sw / w0)
    cols = min(len(es), 8)
    strip = Image.new('RGB', (cols * sw, ((len(es) + cols - 1) // cols) * (sh + 16)), (20, 20, 20))
    dr = ImageDraw.Draw(strip)
    for k, (r, n, count, mask, path) in enumerate(es):
        img = np.asarray(Image.open(path).convert('RGB'))[:, :, ::-1].copy()
        grown = mask.copy()
        for d in (-2, -1, 1, 2):
            grown |= np.roll(mask, d, axis=0) | np.roll(mask, d, axis=1)
        img[grown] = (img[grown] // 3) + np.array([170, 0, 170], dtype=np.uint8)
        x, y = (k % cols) * sw, (k // cols) * (sh + 16)
        strip.paste(Image.fromarray(img).resize((sw, sh)), (x, y))
        dr.text((x + 2, y + sh + 2), f'r{r} s{n}: {count}', fill=(230, 230, 230))
    strip.save(os.path.join(out_dir, f'strip-{key.split()[1][-11:]}-{key.split()[3][-4:]}.png'))
never = [p for p in did_not_draw if p not in best]
for p in never:
    print(f'{"-":>8}  {p}  never drew in its step ({did_not_draw[p]} tries)')

tw, cols = 320, 6
for page in range(0, len(rows), cols * 4):
    rr = rows[page:page + cols * 4]
    w0, h0 = Image.open(rr[0][1][5]).size
    th = int(h0 * tw / w0)
    sheet = Image.new('RGB', (cols * tw, ((len(rr) + cols - 1) // cols) * (th + 26)), (20, 20, 20))
    draw = ImageDraw.Draw(sheet)
    for k, (key, (count, r, n, what, mask, path, _)) in enumerate(rr):
        img = np.asarray(Image.open(path).convert('RGB'))[:, :, ::-1].copy()  # captures are BGR
        grown = mask.copy()
        for d in (-2, -1, 1, 2):
            grown |= np.roll(mask, d, axis=0) | np.roll(mask, d, axis=1)
        img[grown] = (img[grown] // 3) + np.array([170, 0, 170], dtype=np.uint8)
        x, y = (k % cols) * tw, (k // cols) * (th + 26)
        sheet.paste(Image.fromarray(img).resize((tw, th)), (x, y))
        draw.text((x + 3, y + th + 3), f'{key.split()[1][-8:]} r{r} {count}', fill=(230, 230, 230))
    out = os.path.join(out_dir, f'sheet{page // (cols * 4) + 1}.png')
    sheet.save(out)
    print('wrote', out)
