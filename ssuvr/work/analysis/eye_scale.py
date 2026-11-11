"""Eye separation and head-movement scale in game units, from a head-test.ps1 run (2026-11-07).

Usage: eye_scale.py <run folder> [<run folder> ...]
The game keeps one camera block per eye, 0x100 apart; at +0x48 of each (the address found first) a
float3 whose length is the camera distance (275 near) and whose x is the eye's sideways offset (+/- half
the eye separation, sign flipped: it is the view translation). The head moves 10 cm to the right at 62 s,
so the change of that x over the move, divided by 0.1 m, is game units per metre of head movement.
Finds the pair by shape: two stable triples 0x100 apart, same y and z, x of opposite sign, |v| 270-330.
"""
import csv
import sys

import numpy as np

DT = np.dtype([('a', '<u8'), ('v', '<f4', 3)])
for run in sys.argv[1:]:
    s = np.fromfile(f'{run}/ssu-stable.bin', dtype=DT)
    moved = {int(r['address'], 16): r for r in csv.DictReader(open(f'{run}/ssu-moved.csv'))}
    by_addr = {int(a): v for a, v in zip(s['a'], s['v'])}
    print('==', run)
    found = 0
    for a, v in by_addr.items():
        w = by_addr.get(a + 0x100)
        r = float(np.linalg.norm(v))
        if w is None or not (270 < r < 330) or v[0] <= 0.5 or abs(v[0] + w[0]) > 1e-4:
            continue
        if v[1] != w[1] or v[2] != w[2]:
            continue
        found += 1
        sep = float(v[0] - w[0])
        line = f'  {a:x} / {a + 0x100:x}: x {v[0]:+.4f} / {w[0]:+.4f} -> eye separation {sep:.3f} units; |v| {r:.2f}'
        m = moved.get(a)
        if m and m['x1'] not in ('', 'nan'):
            dx = float(m['x1']) - float(m['x0'])
            line += f'; after the 10 cm head move x changed {dx:+.3f} -> {abs(dx) / 0.1:.1f} units per metre'
        print(line)
        if found >= 6:
            break
    if not found:
        print('  no eye pair found')
