"""Shot speed and lifetime per traced slot, in seconds (2026-11-07, weapon checks at 60 vs world 120).

Usage: shot_motion.py <run folder> [<run folder> ...]
Reads the SSU trace (ssu-trace.bin / ssu-trace-addr.txt, SHADPS4_SSU_TRACE). Shots fly at r ~ 140 in a
plane x ~ const, phi = atan2(z, y) rising. Splits each slot's samples into lives (a drop in phi or a gap
over 0.2 s starts a new one) and prints, over the lives of the centre stream (|x| < 3): count, median
speed (deg/s from the first to the last sample of a life), median and 90th-percentile life length (s),
median and max phi reached. Independent of the update rate, so 60 and 120 runs compare directly.
"""
import math
import sys

import numpy as np

REC = np.dtype([('t', '<f4'), ('i', '<u4'), ('v', '<f4', 3)])
for run in sys.argv[1:]:
    rec = np.fromfile(f'{run}/ssu-trace.bin', dtype=REC)
    rec = rec[np.isfinite(rec['v']).all(axis=1)]
    rec = rec[np.lexsort((rec['t'], rec['i']))]
    lives = []
    for ch in np.split(rec, np.flatnonzero(np.diff(rec['i'])) + 1):
        v = ch['v'].astype(float)
        r = np.linalg.norm(v, axis=1)
        ok = (np.abs(r - 140) < 6) & (np.abs(v[:, 0]) < 3)
        t = ch['t'][ok].astype(float)
        phi = np.degrees(np.arctan2(v[ok, 2], v[ok, 1]))
        if len(t) < 3:
            continue
        start = 0
        for k in range(1, len(t) + 1):
            if k == len(t) or phi[k] < phi[k - 1] - 0.5 or t[k] - t[k - 1] > 0.2:
                if k - start >= 3 and phi[k - 1] - phi[start] > 1.0:
                    lives.append((t[k - 1] - t[start], phi[start], phi[k - 1]))
                start = k
    if not lives:
        print(f'== {run}: no lives found')
        continue
    a = np.array(lives)
    dur, p0, p1 = a[:, 0], a[:, 1], a[:, 2]
    speed = (p1 - p0) / np.maximum(dur, 1e-3)
    born = p0 < 5  # lives seen from the muzzle
    print(f'== {run}')
    print(f'  {len(a)} lives ({born.sum()} from the muzzle): speed median {np.median(speed):.1f} deg/s; '
          f'life median {np.median(dur[born]) if born.any() else math.nan:.3f} s, '
          f'p90 {np.percentile(dur[born], 90) if born.any() else math.nan:.3f} s; '
          f'reach median {np.median(p1[born]) if born.any() else math.nan:.1f}, max {p1.max():.1f} deg')
