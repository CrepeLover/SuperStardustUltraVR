"""After sweepdiff.py (pause mode): drops the pixels that change in many steps (animation, not the
dropped pipeline) and recounts each step's own changes. usage: sweepmask.py <shad_log> <out dir>"""
import re
import sys

import numpy as np
from PIL import Image

log_path, out_dir = sys.argv[1:3]
steps, pairs = [], []
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
load = lambda p: np.asarray(Image.open(p).convert('RGB'), dtype=np.int16)
imgs = [(load(p[0]), load(p[1])) for p in pairs]
base = [i for i, s in enumerate(steps) if s[1].startswith('nothing')][0]
THR = 24
masks = [[np.abs(imgs[i][e] - imgs[base][e]).max(axis=2) > THR for e in (0, 1)] for i in range(len(imgs))]
drops = [i for i in range(len(imgs)) if i != base]
for e in (0, 1):
    hits = sum(masks[i][e].astype(np.int32) for i in drops)
    animated = hits >= 6
    print(f'eye {e}: {int(animated.sum())} pixels change in 6+ steps (animated), excluded')
    for i in drops:
        masks[i][e] = masks[i][e] & ~animated
for i in drops:
    n, what = steps[i]
    a, b = int(masks[i][0].sum()), int(masks[i][1].sum())
    ys, xs = np.nonzero(masks[i][0] | masks[i][1])
    where = f'x {xs.min()}-{xs.max()} y {ys.min()}-{ys.max()}' if len(xs) else '-'
    flag = '   <-- NOTHING OF ITS OWN' if a + b < 40 else ''
    print(f'{n:>3} {a:>7} {b:>7}  {where:<22} {what[:92]}{flag}')
h, w, _ = imgs[base][0].shape
print(f'eye picture {w}x{h}')
