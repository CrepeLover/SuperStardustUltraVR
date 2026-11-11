"""Shot sweep from the SSU trace (2026-11-07), frame-independent.

Usage: shot_sweep.py <run folder> [...]
Every traced position at r ~ 140 (the Rock Crusher shots' sphere), grouped by address and cut into lives
(a pause over 0.05 s or a jump over 20 units starts a new one). For each life that lasted at least
0.2 s: the angle between its first and last position (how far round the sphere it got), its lifetime
and its angular speed. Prints medians and spread, so runs at different frame rates compare directly.
"""
import sys
import numpy as np

REC = np.dtype([('t', '<f4'), ('i', '<u4'), ('v', '<f4', 3)])
for run in sys.argv[1:]:
    rec = np.fromfile(f'{run}/ssu-trace.bin', dtype=REC)
    rec = rec[np.isfinite(rec['v']).all(axis=1)]
    v = rec['v'].astype(float)
    r = np.linalg.norm(v, axis=1)
    keep = np.abs(r - 140) < 0.5
    rec, v = rec[keep], v[keep]
    sweeps, lives, speeds, steps = [], [], [], []
    for a in np.unique(rec['i']):
        m = rec['i'] == a
        t = rec['t'][m].astype(float)
        p = v[m]
        o = np.argsort(t, kind='stable')
        t, p = t[o], p[o]
        cut = np.where((np.diff(t) > 0.05) | (np.linalg.norm(np.diff(p, axis=0), axis=1) > 20))[0] + 1
        for s, e in zip(np.r_[0, cut], np.r_[cut, len(t)]):
            if e - s < 5 or t[e - 1] - t[s] < 0.2:
                continue
            u0 = p[s] / np.linalg.norm(p[s])
            u1 = p[e - 1] / np.linalg.norm(p[e - 1])
            ang = np.degrees(np.arccos(np.clip(u0 @ u1, -1, 1)))
            if ang < 2:
                continue
            sweeps.append(ang)
            lives.append(t[e - 1] - t[s])
            speeds.append(ang / (t[e - 1] - t[s]))
            steps.append(np.median(np.diff(t[s:e])))
    print('==', run.replace('\\', '/').split('/')[-1], f'{len(sweeps)} moving lives')
    if sweeps:
        q = lambda x: '%.2f (p10 %.2f, p90 %.2f)' % (np.median(x), np.percentile(x, 10), np.percentile(x, 90))
        print('  swept deg  ', q(sweeps))
        print('  lifetime s ', q(lives))
        print('  speed deg/s', q(speeds))
        print('  update step s %.4f' % np.median(steps))
