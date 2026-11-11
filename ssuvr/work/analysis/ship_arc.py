"""Ship arc length from the SSU probe (2026-11-07).

Usage: ship_arc.py <run folder> [<run folder> ...]
Reads ssu-moved.csv / ssu-track.csv from each run (SHADPS4_SSU_SCAN), picks the position vectors that
started moving with the scripted stick push (66 s) and kept their length, groups identical paths
(the game keeps many copies of the ship's position), and prints for the largest group: the radius,
the total angle and arc, and the speed along the arc in 0.5 s steps.
"""
import csv
import math
import sys
from collections import defaultdict


def num(x):
    try:
        return float(x)
    except ValueError:
        return math.nan


def angle(a, b):
    ra = math.sqrt(sum(v * v for v in a))
    rb = math.sqrt(sum(v * v for v in b))
    c = sum(x * y for x, y in zip(a, b)) / (ra * rb)
    return math.acos(max(-1.0, min(1.0, c)))


for run in sys.argv[1:]:
    print('==', run)
    moved = list(csv.DictReader(open(f'{run}/ssu-moved.csv')))
    for r in moved:
        for k in r:
            if k != 'address':
                r[k] = num(r[k])
    cand = [r for r in moved if 65.0 <= r['first_change'] <= 68 and abs(r['r0'] - r['r1']) < 1.0
            and not math.isnan(r['angle_deg'])]
    groups = defaultdict(list)
    for r in cand:
        groups[(round(r['r0'], 2), round(r['angle_deg'], 1))].append(r)
    ranked = sorted(groups.items(), key=lambda kv: -len(kv[1]))
    for (r0, ang), rs in ranked[:6]:
        print(f'  r={r0:8.2f}  angle={ang:6.1f} deg  copies={len(rs):4d}  first={min(x["first_change"] for x in rs):.2f}'
              f'  last={max(x["last_change"] for x in rs):.2f}')
    # The ship flies at the fly radius (140).
    (r0, ang), rs = next(kv for kv in ranked if 135 < kv[0][0] < 145 and kv[0][1] > 1)
    addr = rs[0]['address']
    path = [(float(x['seconds']), (float(x['x']), float(x['y']), float(x['z'])))
            for x in csv.DictReader(open(f'{run}/ssu-track.csv')) if x['address'] == addr]
    path.sort()
    p0 = path[0][1]
    radius = math.sqrt(sum(v * v for v in p0))
    total = angle(p0, path[-1][1])
    print(f'  ship ({addr}): radius {radius:.3f}, total {math.degrees(total):.2f} deg = arc {total * radius:.1f} units'
          f' = {total / (2 * math.pi) * 100:.1f}% of a great circle')
    # Arc travelled (sum of small steps), and speed per 0.5 s.
    out = []
    travelled = 0.0
    path = [p for p in path if sum(v * v for v in p[1]) > 1]
    for (t0, a), (t1, b) in zip(path, path[1:]):
        step = angle(a, b) * radius
        travelled += step
        out.append((t1, travelled))
    print(f'  path length {travelled:.1f} units')
    line = []
    for t in [x / 2 for x in range(130, 168)]:
        before = [d for tt, d in out if tt <= t]
        after = [d for tt, d in out if tt <= t + 0.5]
        if before and after:
            line.append(f'{t:.1f}:{(after[-1] - before[-1]) / 0.5:.0f}')
    print('  speed (units/s) from t:', ' '.join(line))
    # Distance per scripted segment (boost-test.ps1: move 66-69.5, boost 71-75.5).
    for name, a, b in (('move 66-69.5', 66.0, 69.5), ('boost 71-75.5', 71.0, 75.5)):
        da = [d for tt, d in out if tt <= a]
        db = [d for tt, d in out if tt <= b]
        if da and db:
            print(f'  {name}: {db[-1] - da[-1]:.1f} units')
