"""Rock Crusher reach from the SSU trace, by angle in the shot's own frame (2026-11-07).

Usage: shot_reach.py <run folder> [<run folder> ...]
The game keeps each shot in its own frame: it sits in that frame's y-z plane (x = 0) at r = 140, born at
the top (angle phi = atan2(z, y) of a degree or so: the gun is just ahead of the ship) and sweeping up in
phi until it dies; the slot then restarts near 0. So per address: phi over time, a new life wherever phi
drops; reach = the largest phi of a life, speed = slope of phi over time within a life.
"""
import math
import sys

import numpy as np

REC = np.dtype([('t', '<f4'), ('i', '<u4'), ('v', '<f4', 3)])

for run in sys.argv[1:]:
    rec = np.fromfile(f'{run}/ssu-trace.bin', dtype=REC)
    t_begin, t_end = float(rec['t'].min()), float(rec['t'].max())
    rec = rec[np.isfinite(rec['v']).all(axis=1)]
    rec = rec[np.lexsort((rec['t'], rec['i']))]
    reach, slopes, births, samples = [], [], [], []
    for ch in np.split(rec, np.flatnonzero(np.diff(rec['i'])) + 1):
        v = ch['v'].astype(float)
        r = np.linalg.norm(v, axis=1)
        plane = (np.abs(v[:, 0]) < 1e-4) & (np.abs(r - 140.0) < 0.05)
        if plane.sum() < 5 or plane.mean() < 0.9:
            continue
        t = ch['t'][plane].astype(float)
        phi = np.degrees(np.arctan2(v[plane, 2], v[plane, 1]))
        cut = np.flatnonzero(np.diff(phi) < -2.0) + 1
        b = [0, *cut.tolist(), len(phi)]
        for a, e in zip(b, b[1:]):
            # Only lives seen from birth (not the first one, which may have started before tracing)
            # to death (not the last one, which may go on after it).
            if a == 0 or e == len(phi) or e - a < 3:
                continue
            reach.append(phi[a:e].max())
            births.append(phi[a])
            samples.append(e - a)
            if e - a >= 4:
                slopes.append(np.polyfit(t[a:e], phi[a:e], 1)[0])
    print(f'== {run}  (traced {t_begin:.2f}..{t_end:.2f} s)')
    if not reach:
        print('  no shot lives found')
        continue
    reach = np.array(reach)
    slopes = np.array(slopes)
    print(f'  {len(reach)} complete lives, {np.median(samples):.0f} samples each (median)')
    print(f'  born at phi (deg): {np.round(np.percentile(births, [10, 50, 90]), 2)}')
    print(f'  reach (deg): max {reach.max():.2f}, p99 {np.percentile(reach, 99):.2f}, '
          f'p90 {np.percentile(reach, 90):.2f}, median {np.median(reach):.2f}')
    print(f'  arc at max reach: {math.radians(reach.max()) * 140:.1f} units on the r = 140 sphere '
          f'({reach.max() / 360 * 100:.1f}% of a great circle)')
    if len(slopes):
        s = np.median(slopes)
        print(f'  speed: {s:.1f} deg/s = {math.radians(s) * 140:.0f} units/s (median of {len(slopes)} fits); '
              f'life to max reach ~ {reach.max() / s:.3f} s')
    hist = np.histogram(reach, np.arange(0, 95, 5))[0]
    print('  reach histogram, 5 deg bins:', ' '.join(f'{lo}:{n}' for lo, n in zip(range(0, 95, 5), hist) if n))
