"""Rock Crusher reach per stream from the SSU trace (2026-11-07).

Usage: shot_streams.py <run folder> [<run folder> ...]
Shots live in their own frame at r = 140 in a plane x = const (0 for the single centre stream), born near
phi = atan2(z, y) = 0 and sweeping up in phi. Per stream offset x: the furthest phi any slot reached, the
slots, and the per-frame step; plus the per-address phi ranges for the slots of the centre stream.
"""
import sys
from collections import defaultdict
import math
import numpy as np

REC = np.dtype([('t', '<f4'), ('i', '<u4'), ('v', '<f4', 3)])
for run in sys.argv[1:]:
    rec = np.fromfile(f'{run}/ssu-trace.bin', dtype=REC)
    addrs = open(f'{run}/ssu-trace-addr.txt').read().split()
    t0, t1 = rec['t'].min(), rec['t'].max()
    rec = rec[np.isfinite(rec['v']).all(axis=1)]
    rec = rec[np.lexsort((rec['t'], rec['i']))]
    found = defaultdict(list)
    for ch in np.split(rec, np.flatnonzero(np.diff(rec['i'])) + 1):
        v = ch['v'].astype(float)
        if len(v) < 15:
            continue
        r = np.linalg.norm(v, axis=1)
        ok = np.abs(r - 140.0) < 0.05
        if ok.mean() < 0.9:
            continue
        xs = np.round(v[ok, 0], 2)
        vals, counts = np.unique(xs, return_counts=True)
        if counts.max() < 0.9 * ok.sum():
            continue
        xc = vals[counts.argmax()]
        if abs(xc) > 30:
            continue
        phi = np.degrees(np.arctan2(v[ok, 2], v[ok, 1]))
        if phi.max() - phi.min() < 3:
            continue
        steps = np.diff(phi)
        fwd = steps[(steps > 0.05) & (steps < 5)]
        found[xc].append((addrs[ch['i'][0]], phi.min(), phi.max(), ok.sum(), np.median(fwd) if len(fwd) else math.nan,
                          ch['t'][ok].min(), ch['t'][ok].max()))
    print(f'== {run}  traced {t0:.2f}..{t1:.2f} s')
    for xc in sorted(found):
        rows = found[xc]
        print(f'  stream x = {xc:6.2f}: {len(rows)} addresses, furthest phi {max(r[2] for r in rows):.2f} deg '
              f'(= {math.radians(max(r[2] for r in rows)) * 140:.1f} units of arc), step/frame {np.nanmedian([r[4] for r in rows]):.3f} deg, '
              f'seen {min(r[5] for r in rows):.1f}..{max(r[6] for r in rows):.1f} s')
