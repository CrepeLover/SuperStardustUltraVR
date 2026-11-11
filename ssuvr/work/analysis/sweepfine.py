"""Fine look at chosen steps of a paused SHADPS4_DEBUG_SWEEP=..,alt run: per-pixel difference to the
nearest nothing-dropped capture, as counts above small thresholds. usage: sweepfine.py <log> <step>..."""
import re
import sys

import numpy as np
from PIL import Image

log_path = sys.argv[1]
want = [int(s) for s in sys.argv[2:]]
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
load = lambda p: np.asarray(Image.open(p).convert('RGB'), dtype=np.int16)
bases = [k for k, s in enumerate(steps) if s[1].startswith('nothing') and len(pairs[k]) == 2]
B = [load(pairs[k][0]) for k in bases]
# Nothing-dropped against nothing-dropped (leave one out): the floor.
floor = []
for j in range(min(6, len(B))):
    d = np.min([np.abs(B[j] - B[k]).max(axis=2) for k in range(len(B)) if k != j], axis=0)
    floor.append([int((d > t).sum()) for t in (1, 2, 4, 8)])
print('nothing-dropped vs the others (px > 1, 2, 4, 8):', floor)
for k, (n, what) in enumerate(steps):
    if n not in want or len(pairs[k]) != 2:
        continue
    img = load(pairs[k][0])
    d = np.min([np.abs(img - b).max(axis=2) for b in B], axis=0)
    print(f'step {n}: px > 1, 2, 4, 8: {[int((d > t).sum()) for t in (1, 2, 4, 8)]}, max {int(d.max())} | {what[:80]}')
