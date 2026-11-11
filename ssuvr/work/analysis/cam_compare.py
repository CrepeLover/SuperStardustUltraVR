"""Camera-radius motion compare from SSU traces (2026-11-07).

Usage: cam_compare.py <run A> <run B> [radius lo-hi]
For each run: traced triples whose length stays in the radius band (default 270-330), grouped by
address. Picks the addresses that move most (angle swept) and prints, for the top few, the angle
from the start position against time since motion onset (first time it is 0.5 deg away), in
0.1 s steps, so a per-frame lerp (different lag at a different draw rate) shows as a different
curve while the end angle stays the same.
"""
import sys
import numpy as np

REC = np.dtype([('t', '<f4'), ('i', '<u4'), ('v', '<f4', 3)])
lo, hi = (270.0, 330.0) if len(sys.argv) < 4 else map(float, sys.argv[3].split('-'))


def tracks(run):
    rec = np.fromfile(f'{run}/ssu-trace.bin', dtype=REC)
    rec = rec[np.isfinite(rec['v']).all(axis=1)]
    v = rec['v'].astype(float)
    r = np.linalg.norm(v, axis=1)
    keep = (r > lo) & (r < hi)
    rec, v = rec[keep], v[keep]
    out = []
    for a in np.unique(rec['i']):
        m = rec['i'] == a
        if m.sum() < 30:
            continue
        t = rec['t'][m].astype(float)
        p = v[m]
        o = np.argsort(t, kind='stable')
        t, p = t[o], p[o]
        u = p / np.linalg.norm(p, axis=1)[:, None]
        ang = np.degrees(np.arccos(np.clip(u @ u[0], -1, 1)))
        out.append((ang.max(), a, t, ang, np.linalg.norm(p, axis=1).mean()))
    out.sort(key=lambda x: -x[0])
    return out


for run in sys.argv[1:3]:
    tr = tracks(run)
    print('==', run.replace('\\', '/').split('/')[-1], f'{len(tr)} tracks in {lo:g}-{hi:g}')
    shown = 0
    for sweep, a, t, ang, rad in tr:
        if sweep < 2 or t[-1] - t[0] < 3:
            continue
        on = np.argmax(ang > 0.5)
        t0 = t[on]
        grid = np.arange(-0.2, 3.01, 0.2)
        curve = np.interp(t0 + grid, t, ang)
        print(f'  addr #{a} r {rad:.1f} sweep {sweep:.2f} deg, onset {t0:.2f} s, samples {len(t)}, '
              f'step {np.median(np.diff(t)):.4f} s')
        print('    deg at +s: ' + ' '.join(f'{g:+.1f}:{c:.2f}' for g, c in zip(grid, curve)))
        shown += 1
        if shown >= 4:
            break
