"""Gold Melter beam shape while the aim turns (2026-11-07): reads ssu-beam.csv (SHADPS4_SSU_BEAM=1 probe:
script seconds, live segments, chains, then the longest chain's positions from its tip to its newest end).
Bend = angle at the newest end between the first gap and the chord to the tip (0 = straight).
Usage: beam_bend.py <run> [<run> ...]"""
import sys
import numpy as np

for run in sys.argv[1:]:
    rows = []
    for line in open(f'{run}/ssu-beam.csv'):
        f = line.split(',')
        t = float(f[0])
        pts = np.array([float(x) for x in f[3:]]).reshape(-1, 3)
        if t < 64 or len(pts) < 25:
            continue
        a = pts[-2] - pts[-1]
        b = pts[0] - pts[-1]
        bend = np.degrees(np.arccos(np.clip(a @ b / np.linalg.norm(a) / np.linalg.norm(b), -1, 1)))
        u = pts / np.linalg.norm(pts, axis=1, keepdims=True)
        reach = np.degrees(np.arccos(np.clip(u[0] @ u[-1], -1, 1)))
        length = np.linalg.norm(np.diff(pts, axis=0), axis=1).sum()
        rows.append((t, len(pts), bend, reach, length))
    a = np.array(rows)
    print(f'== {run}: {len(a)} full-length samples')
    for name, col in (('segments', 1), ('bend deg', 2), ('reach deg', 3), ('length', 4)):
        print(f'   {name:9s} p10 {np.percentile(a[:, col], 10):6.1f} p50 {np.median(a[:, col]):6.1f} p90 {np.percentile(a[:, col], 90):6.1f}')
