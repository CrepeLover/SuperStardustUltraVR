"""Shot range from the SSU trace (2026-11-07).

Usage: shot_range.py <run folder> [<run folder> ...]
Reads ssu-trace.bin / ssu-trace-addr.txt (SHADPS4_SSU_TRACE): every change of the positions that came
and went while the ship stood still and fired. Each address's path is cut into lives (a jump of more than
20 units or a pause of more than 0.25 s starts a new one). The game keeps each Rock Crusher shot in its
own frame: it starts at (0, 140, 0) (or (140, 0, 0) seen one float earlier) and sweeps round the
r = 140 sphere until it dies. Prints, for the lives that start at the top of that frame and move, how far
round the sphere they got (angle and arc), how long they lived and their angular speed.
"""
import math
import sys
from collections import Counter

import numpy as np

REC = np.dtype([('t', '<f4'), ('i', '<u4'), ('v', '<f4', 3)])


def lives_of(run):
    rec = np.fromfile(f'{run}/ssu-trace.bin', dtype=REC)
    addrs = open(f'{run}/ssu-trace-addr.txt').read().split()
    t0, t1 = float(rec['t'].min()), float(rec['t'].max())
    rec = rec[np.isfinite(rec['v']).all(axis=1) & (np.abs(rec['v']) < 1e4).all(axis=1)]
    rec = rec[np.lexsort((rec['t'], rec['i']))]
    out = []
    for ch in np.split(rec, np.flatnonzero(np.diff(rec['i'])) + 1):
        v = ch['v'].astype(float)
        t = ch['t'].astype(float)
        r = np.linalg.norm(v, axis=1)
        step = np.linalg.norm(np.diff(v, axis=0), axis=1)
        cut = np.flatnonzero((step > 20) | (np.diff(t) > 0.25)) + 1
        b = [0, *cut.tolist(), len(ch)]
        for a, e in zip(b, b[1:]):
            if e - a < 6:
                continue
            rr = r[a:e]
            if rr.min() < 120 or rr.max() > 160 or rr.std() > 1.5:
                continue
            u0, u1 = v[a] / rr[0], v[e - 1] / rr[-1]
            out.append(dict(addr=addrs[ch['i'][0]], ts=t[a], te=t[e - 1], n=e - a, r=float(rr.mean()),
                            s=v[a], e=v[e - 1], ang=math.degrees(math.acos(max(-1.0, min(1.0, float(u0 @ u1))))),
                            trunc=t[a] <= t0 + 1e-3 or t[e - 1] >= t1 - 0.05))
    return out, (t0, t1), len(addrs)


for run in sys.argv[1:]:
    lives, (t0, t1), naddr = lives_of(run)
    print(f'== {run}\n  {naddr} addresses traced {t0:.2f}..{t1:.2f} s; {len(lives)} smooth lives at r 120-160')
    top = [l for l in lives if not l['trunc'] and l['ang'] > 3
           and (np.linalg.norm(l['s'] - [0, 140, 0]) < 1e-3 or np.linalg.norm(l['s'] - [140, 0, 0]) < 1e-3)]
    print(f'  {len(top)} complete lives start exactly at the top of their frame (0,140,0) and move')
    if not top:
        continue
    ang = np.array([l['ang'] for l in top])
    dur = np.array([l['te'] - l['ts'] for l in top])
    rad = np.array([l['r'] for l in top])
    print(f'  radius {np.median(rad):.3f}; reach (deg): max {ang.max():.2f}, p90 {np.percentile(ang, 90):.2f}, '
          f'median {np.median(ang):.2f}; arc at max {math.radians(ang.max()) * np.median(rad):.1f} units')
    print(f'  life (s): max {dur.max():.3f}, median {np.median(dur):.3f}; angular speed (deg/s): median '
          f'{np.median(ang / np.maximum(dur, 1e-3)):.1f} = {math.radians(np.median(ang / np.maximum(dur, 1e-3))) * np.median(rad):.0f} units/s')
    ends = Counter((round(l['ang'], 1), round(l['te'] - l['ts'], 2)) for l in top)
    print('  most common (reach deg, life s):', ends.most_common(6))
    print('  reach histogram, 5 deg bins:', ' '.join(f'{int(lo)}:{n}' for lo, n in
                                                    zip(np.arange(0, 95, 5), np.histogram(ang, np.arange(0, 100, 5))[0]) if n))
