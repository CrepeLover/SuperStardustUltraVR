"""Static check of dumped pixel shaders (dump_shaders=true IR dumps) for the point-sprite bug class:
barycentric coordinates used as data (texture coordinates, maths) instead of only interpolating
vertex attributes (FPFma32 with a per-vertex difference). Such shaders are exact on triangles but
depend on how the GPU defines barycentrics for points, lines, and emulated quads/rects.
usage: barycheck.py <dumps dir>"""
import os
import re
import sys

dumps = sys.argv[1]
for name in sorted(os.listdir(dumps)):
    if not (name.startswith('fs_') and name.endswith('.irprogram.txt') and name.count('.') == 2):
        continue
    lines = open(os.path.join(dumps, name), encoding='utf-8', errors='replace').read().splitlines()
    defs = {}
    for line in lines:
        m = re.search(r'(%\d+)\s+= GetAttribute (BaryCoord\w+), #(\d)', line)
        if m:
            defs[m.group(1)] = f'{m.group(2)}.{m.group(3)}'
    if not defs:
        continue
    odd = []
    for line in lines:
        m = re.search(r'(%\d+)?\s*=?\s*(\w+)\s+(.*)$', line.split('] ', 1)[-1])
        body = line.split('] ', 1)[-1]
        op = re.search(r'=\s+(\w+)', body)
        opname = op.group(1) if op else body.split()[0] if body.split() else ''
        for ref, what in defs.items():
            if re.search(re.escape(ref) + r'\b', body) and not body.lstrip().startswith(ref + ' '):
                if opname != 'FPFma32':
                    odd.append(f'{what} -> {opname}')
    kinds = sorted(set(defs.values()))
    print(f'{name[:22]} reads {",".join(kinds)}' + (f'  AS DATA: {sorted(set(odd))}' if odd else ''))
