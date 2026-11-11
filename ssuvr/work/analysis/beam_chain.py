"""Gold Melter beam length per trace sweep (2026-11-07). Usage: beam_chain.py <run> [...]
Takes points on the globe (|r-140| < 0.5) in the ship's firing plane (|x| < 0.05) in each sweep, walks up from the
muzzle (phi < 4) while the gap to the next point is under 3 deg, and reports the chain's reach and point spacing."""
import sys
import numpy as np
REC = np.dtype([('t', '<f4'), ('i', '<u4'), ('v', '<f4', 3)])
for run in sys.argv[1:]:
    rec = np.fromfile(f'{run}/ssu-trace.bin', dtype=REC)
    rec = rec[np.isfinite(rec['v']).all(axis=1)]
    rec = rec[np.argsort(rec['t'], kind='stable')]
    t = rec['t'].astype(float); v = rec['v'].astype(float); r = np.linalg.norm(v, axis=1)
    keep = (np.abs(r - 140) < 0.5) & (np.abs(v[:, 0]) < 0.05)
    reach, npts, gaps = [], [], []
    for s in np.split(np.arange(len(t)), np.flatnonzero(np.diff(t) > 0.002) + 1):
        sel = s[keep[s]]
        phi = np.unique(np.round(np.degrees(np.arctan2(v[sel, 2], v[sel, 1])), 3))
        phi = phi[(phi > -5) & (phi < 179)]
        if len(phi) < 3 or phi[0] > 4:
            continue
        d = np.diff(phi)
        end = np.flatnonzero(d > 3)
        n = (end[0] + 1) if len(end) else len(phi)
        reach.append(phi[n - 1]); npts.append(n)
        if n > 5: gaps.append(np.median(d[:n - 1]))
    reach, npts = np.array(reach), np.array(npts)
    long = reach > 20
    print(f'{run[-26:]:28s} sweeps {len(reach):4d}; reach p50 {np.median(reach):5.1f} p90 {np.percentile(reach, 90):5.1f} '
          f'max {reach.max():5.1f} deg; sweeps reaching 85+: {np.mean(reach > 85):.0%}; points/chain (reach>20) '
          f'p50 {np.median(npts[long]) if long.any() else 0:.0f}; spacing p50 {np.median(gaps):.3f} deg')
