"""Bullet lives per type from the bullet probe (2026-11-08, enemy shots at 60 vs world 120).

Usage: bullet_lives.py <run folder> [<run folder> ...]
Reads <run>\\ssu-bullets.csv (SHADPS4_SSU_BULLETS=1 with SHADPS4_SSU_COUNT=26f920; one line per live pool record
every ~20 ms: t,slot,type,u0,state,age,px,py,pz,dx,dy,dz,effect,u50,u54). The pool is packed, so a slot is not an
identity: a life is a (type, effect handle) whose age keeps rising; without a handle, (type, birth time = t - age).
Per type prints: lives, births per second of play, median radius, median speed (deg/s about the globe's centre and
units/s along the path), life = last age seen (p50 / p90 / max), and reach (angle from the first to the last
position) of lives seen within 0.05 s of birth. Rate-independent, so 60 and 120 runs compare directly.
"""
import sys
from collections import defaultdict

import numpy as np

NAMES = {0: 'plasma1 (player)', 1: 'plasma2 (player)', 2: 'plasma3 (player)', 6: 'plasma_lurker', 7: 'boss2 ray ind.',
         8: 'boss2 ray', 20: 'turret'}


def lives_of(path):
    rows = defaultdict(list)
    with open(path) as f:
        next(f)
        for line in f:
            p = line.rstrip().split(',')
            if len(p) != 15:
                continue
            try:
                t, typ, state, age = float(p[0]), int(p[2]), int(p[4]), float(p[5])
                pos = (float(p[6]), float(p[7]), float(p[8]))
            except ValueError:
                continue
            if not np.isfinite(age) or not all(np.isfinite(pos)) or age < 0 or age > 600:
                continue
            eff = p[12]
            key = (typ, eff) if eff not in ('0', '') else (typ, round(t - age, 2))
            rows[key].append((t, age, *pos, state))
    lives = defaultdict(list)  # type -> list of arrays
    for (typ, _), r in rows.items():
        a = np.array(sorted(set(r)))
        # a handle can be reused: split where the age drops
        cuts = np.flatnonzero(np.diff(a[:, 1]) < -1e-3) + 1
        for seg in np.split(a, cuts):
            if len(seg) >= 2:
                lives[typ].append(seg)
    return lives


def angle(u, v):
    c = np.dot(u, v) / max(np.linalg.norm(u) * np.linalg.norm(v), 1e-9)
    return np.degrees(np.arccos(np.clip(c, -1, 1)))


for run in sys.argv[1:]:
    lives = lives_of(f'{run}/ssu-bullets.csv')
    allt = [s[:, 0] for v in lives.values() for s in v]
    span = (max(x.max() for x in allt) - min(x.min() for x in allt)) if allt else 0
    print(f'== {run}  ({span:.0f} s with bullets)')
    print(f'  {"type":>4} {"name":18s} {"lives":>5} {"/s":>5} {"r":>6} {"deg/s":>7} {"u/s":>6} '
          f'{"life p50":>8} {"p90":>6} {"max":>6} {"reach p50":>9} {"p90":>6} {"max":>6}')
    for typ in sorted(lives):
        L = lives[typ]
        r = np.median([np.median(np.linalg.norm(s[:, 2:5], axis=1)) for s in L])
        spd, ups, life, reach = [], [], [], []
        for s in L:
            dt = s[-1, 1] - s[0, 1]
            life.append(s[-1, 1])
            if dt > 0.05:
                spd.append(angle(s[0, 2:5], s[-1, 2:5]) / dt)
                ups.append(np.linalg.norm(np.diff(s[:, 2:5], axis=0), axis=1).sum() / dt)
            if s[0, 1] < 0.05:
                reach.append(angle(s[0, 2:5], s[-1, 2:5]))
        q = lambda v, p: np.percentile(v, p) if len(v) else np.nan  # noqa: E731
        print(f'  {typ:4d} {NAMES.get(typ, ""):18s} {len(L):5d} {len(L) / max(span, 1):5.2f} {r:6.1f} {q(spd, 50):7.1f} '
              f'{q(ups, 50):6.1f} {q(life, 50):8.3f} {q(life, 90):6.3f} {max(life):6.3f} {q(reach, 50):9.1f} '
              f'{q(reach, 90):6.1f} {max(reach) if reach else np.nan:6.1f}')
