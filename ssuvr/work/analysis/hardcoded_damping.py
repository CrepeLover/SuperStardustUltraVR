"""Hard-coded per-update vector dampings (2026-11-07, variable frame rate work).

Usage: hardcoded_damping.py [lo hi]   (code range, default the whole executable)
hardcoded_lerps.py misses a constant that scales a vector: vmovss xmmK, [rip + c] (0.5 < c < 1) then
two or more vmulss using xmmK as a source (x, y, z of a velocity), or the same [rip + c] multiplied
into three registers in a row. Example: the ship's velocity *0.95 / *0.9 each update in sub_31b6b0
(0x31cd76 / 0x31cdbd). Prints candidates (load address, constant, the multiplies) for reading by hand.
"""
import re
import struct
import sys
from collections import defaultdict

sys.path.insert(0, r'C:\Games\ssuvr-work\analysis\refute-c3')
from common import *  # noqa: E402,F401

c = ctx()
lo, hi = (int(sys.argv[1], 16), int(sys.argv[2], 16)) if len(sys.argv) > 2 else (0, 0x6e4e68)
RIP = re.compile(r'rip \+ (0x[0-9a-f]+)\]')


def const_at(x):
    m = RIP.search(x.op_str)
    if not m:
        return None, None
    t = x.address + x.size + int(m.group(1), 16)
    if not (0x590000 <= t < 0x640000):
        return None, None
    b = c.elf.read(t, 4)
    if b is None:
        return None, None
    return t, struct.unpack('<f', b)[0]


found = []
for fs, fe in c.funcs:
    if not (lo <= fs < hi) or fe - fs > 200000:
        continue
    ins = list(c.md.disasm(c.elf.read(fs, fe - fs), fs))
    for i, x in enumerate(ins):
        if x.mnemonic not in ('vmovss', 'vmulss'):
            continue
        t, v = const_at(x)
        if t is None or not (0.5 < v < 1.0):
            continue
        if x.mnemonic == 'vmovss':
            k = x.op_str.split(',')[0]
            muls = []
            for y in ins[i + 1:i + 14]:
                regs = re.findall(r'xmm\d+', y.op_str)
                if y.mnemonic == 'vmulss' and k in regs[1:]:
                    muls.append(y)
                elif regs and regs[0] == k and not y.op_str.startswith('dword'):
                    break  # xmmK overwritten
                elif k in regs[1:] or (y.op_str.startswith('dword') and k in regs):
                    pass  # other use (store / add) - keep looking
            if len(muls) >= 2:
                found.append((fs, x.address, v, t, [f'{m.address:#x} {m.op_str}' for m in muls]))
        else:
            # vmulss a, a, [rip + c] three times in a row (same c): x, y, z
            run = [x]
            for y in ins[i + 1:i + 6]:
                ty, vy = const_at(y)
                if y.mnemonic == 'vmulss' and ty == t:
                    run.append(y)
            prev_same = any(const_at(p)[0] == t and p.mnemonic == 'vmulss' for p in ins[max(0, i - 5):i])
            if len(run) >= 3 and not prev_same:
                found.append((fs, x.address, v, t, [f'{m.address:#x} {m.op_str}' for m in run]))

by_func = defaultdict(list)
for f in found:
    by_func[f[0]].append(f)
for fs in sorted(by_func):
    for _, a, v, t, muls in by_func[fs]:
        print(f'sub_{fs:x} {a:#x} {v:g} @{t:#x}: ' + '; '.join(muls))
print(f'{len(found)} candidates in {len(by_func)} functions', file=sys.stderr)
