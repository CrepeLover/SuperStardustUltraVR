"""Rescale table for running the world at another rate (2026-11-07).

Usage: tunable_table.py [--cpp]
From tunables_all.tsv + tunable_patterns.txt: the registered values the game applies once per world
update without the time step, with how to rescale each for a world rate R (k = 59.94 / R):
  moved     a lerp fraction f (x += (to - x) * f):      f' = 1 - (1 - f)^k
  retained  a kept fraction m (x = to + (x - to) * m):  m' = m^k
  count     frames:                                      n' = n / k
  linear    an increment a frame:                        a' = a * k
Excluded: Bullet Physics dampings (setDamping, applied per second), scales, times, alphas, forces.
Prints the table for review, or with --cpp the C++ initializer for wait_stats.cpp.
"""
import csv
import re
import sys

rows = list(csv.DictReader(open(r'C:\Games\ssuvr-work\analysis\tunables_all.tsv'), delimiter='\t'))
pattern_of = {}
for line in open(r'C:\Games\ssuvr-work\analysis\tunable_patterns.txt'):
    parts = line.split()
    if len(parts) > 3:
        pattern_of.setdefault('sd3__' + parts[2], set()).add(parts[0])

EXCLUDE = re.compile(
    r'COMMAND_GAME__\w+__(linearDamping|angularDamping|EMP_\w+|GRAV\w+|linear_damping)|'
    r'tail_extra_scale|engine_start_health|impact_force|spawn_factor|power_factor|show_time_mul|'
    r'keep_marker|Singularity|cap_fade|fade_time|_time$|seconds|music|text_scale|alpha|fade_value|'
    r'fader_value|screen_fader|fade_speed|fade_in_speed|fade_out_speed|player_dead|view_disabled|'
    r'end_of_round|explode_force|collision_radius|scale_3d|Multiplier_(Scale|X|Y|Z|Timeout)|Score_Display|tilt__angle_mul|'
    r'volume_mul|time_until_turn|time_to_turn|range_from|damp_factor|time_dead_to_rumble|'
    r'BOBBLE_HEAD|sound_volume|boost_effect__velocity_multiplier|approach_multiplier', re.I)
RETAINED = re.compile(r'slowdown|friction|decay|fade_mul|per_frame_mul|dampening|wobble_mul|'
                      r'velocity_mul|power_up_speed_mul|avoid_move_back_mul|filter_lerp', re.I)
MOVED = re.compile(r'lerp|approach_mul|snap_mul|repulsion_fix_mul|speed_down_mul|speed_up_mul', re.I)

table = {}
for r in rows:
    name = r['name']
    if name in table or EXCLUDE.search(name):
        continue
    try:
        v = float(r['default'])
    except ValueError:
        v = None
    low = name.lower()
    kind = None
    if 'frames' in low:
        kind = 'count'
    elif 'per_frame' in low and '_mul' not in low and v is not None and v > 0:
        kind = 'linear'
    elif v is not None and 0 < v < 1:
        if '_lerp' in low and 'filter_lerp' not in low:
            kind = 'moved'
        elif RETAINED.search(name):
            kind = 'retained'
        elif MOVED.search(name):
            kind = 'moved'
    if kind:
        table[name] = (kind, int(r['address'], 16), v, ','.join(sorted(pattern_of.get(name, []))))

# 2026-11-08, from tunable_increments.py, read by hand (kept by hand at the end of the C++ table too).
MANUAL = {
    'sd3__COMMAND_GAME__CARRIER__COMBAT__BULLET__BOMB__FALLOFF__HORIZONTAL': ('retained', 0x9b707c, 0.99),
    'sd3__COMMAND_GAME__CARRIER__COMBAT__BULLET__BOMB__FALLOFF__VERTICAL': ('linear', 0x9b709c, 0.005),
    'sd3__game__enemies__swarmer__boid_move_center_weight': ('linear', 0x9baa64, 0.04),
    'sd3__game__enemies__swarmer__boid_match_velocity_weight': ('moved', 0x9baa84, 0.05),
    'sd3__game__enemies__swarmer__boid_avoid_close_weight': ('linear', 0x9baaa4, 0.2),
    'sd3__game__enemies__swarmer__boid_avoid_close_bullet_weight': ('linear', 0x9baac4, 0.6),
    'sd3__COMMAND_GAME__SHIP__GATLING__BARRELS__ACCEL_SPEED': ('linear', 0x9b3474, 0.05),
}
for name, (kind, addr, v) in MANUAL.items():
    table.setdefault(name, (kind, addr, v, 'manual'))

if '--cpp' in sys.argv:
    for name, (kind, addr, v, pats) in sorted(table.items(), key=lambda kv: kv[1][1]):
        k = {'moved': 'Moved', 'retained': 'Retained', 'count': 'Count', 'linear': 'Linear'}[kind]
        print(f'        {{{addr:#x}, Rescale::{k}, "{name.replace("sd3__", "")}"}},')
else:
    for name, (kind, addr, v, pats) in sorted(table.items()):
        print(f'{kind:9s} {"" if v is None else "%g" % v:>6s} {addr:#x} {name.replace("sd3__", ""):70s} {pats}')
    print(len(table), 'values', file=sys.stderr)
