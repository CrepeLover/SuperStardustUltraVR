"""Gold Melter beam reach from an SSU trace taken while the aim turns (2026-11-07, script-weapon2-sweep.txt).

Usage: beam_reach.py <run folder> [<run folder> ...]
The trace logs only changes, so every 50 ms the latest position of each address that changed in the last 35 ms is
taken as a snapshot (a turning beam moves every update). Points on the flying sphere (|r - 140| < 1) are joined
into chains where neighbours are under LINK degrees apart, starting from points within 4 degrees of the ship
(at the +y pole; the ship sits still). Reach = the largest angle from the ship in the chain. Prints the reach
distribution, the chain's point count and median neighbour spacing, so 60 and 120 runs compare directly.
"""
import sys

import numpy as np

REC = np.dtype([('t', '<f4'), ('i', '<u4'), ('v', '<f4', 3)])
LINK = 3.0
SHIP = np.array([0.0, 1.0, 0.0])


def chain(u):
    """u: unit vectors. Returns (reach deg, n points, median spacing deg) of the chain grown from the ship."""
    d0 = np.degrees(np.arccos(np.clip(u @ SHIP, -1, 1)))
    seeds = np.flatnonzero(d0 < 4)
    if not len(seeds):
        return None
    cos_link = np.cos(np.radians(LINK))
    inside = np.zeros(len(u), bool)
    inside[seeds] = True
    front = seeds
    while len(front):
        near = (u @ u[front].T) > cos_link  # (all points) x (front)
        new = near.any(axis=1) & ~inside
        inside |= new
        front = np.flatnonzero(new)
    c = u[inside]
    order = np.argsort(d0[inside])
    s = np.degrees(np.arccos(np.clip(np.sum(c[order][1:] * c[order][:-1], axis=1), -1, 1)))
    return d0[inside].max(), inside.sum(), (np.median(s) if len(s) else np.nan)


for run in sys.argv[1:]:
    rec = np.fromfile(f'{run}/ssu-trace.bin', dtype=REC)
    rec = rec[np.isfinite(rec['v']).all(axis=1)]
    rec = rec[np.argsort(rec['t'], kind='stable')]
    t = rec['t'].astype(float)
    rows = []
    for T in np.arange(t[0] + 0.5, t[-1], 0.05):
        lo, hi = np.searchsorted(t, [T - 0.035, T])
        w = rec[lo:hi][::-1]
        _, first = np.unique(w['i'], return_index=True)  # latest value per address
        v = w['v'][first].astype(float)
        r = np.linalg.norm(v, axis=1)
        v = v[np.abs(r - 140) < 1]
        if len(v) < 3:
            continue
        res = chain(v / np.linalg.norm(v, axis=1, keepdims=True))
        if res:
            rows.append((T, *res))
    a = np.array(rows)
    reach, n, sp = a[:, 1], a[:, 2], a[:, 3]
    print(f'== {run}')
    print(f'  {len(a)} snapshots: reach p10 {np.percentile(reach, 10):5.1f} p50 {np.median(reach):5.1f} '
          f'p90 {np.percentile(reach, 90):5.1f} max {reach.max():5.1f} deg; points p50 {np.median(n):.0f}; '
          f'spacing p50 {np.nanmedian(sp):.3f} deg')
    print('  reach over time (0.5 s bins):', ' '.join(f'{np.median(reach[(a[:, 0] >= b) & (a[:, 0] < b + 0.5)]):.0f}'
                                                    for b in np.arange(a[0, 0], a[-1, 0], 0.5)
                                                    if ((a[:, 0] >= b) & (a[:, 0] < b + 0.5)).any()))
