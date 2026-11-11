"""Hard-coded per-update lerps / decays (2026-11-07, variable frame rate work).

Usage: hardcoded_lerps.py [lo hi]   (code range, default the game logic 0x1f0000-0x360000)
Finds rip-relative rodata floats in (0, 1) used as
  moved:    vsub a,b ; vmul a,[c] ; vadd a,b      (x += (to - x) * c)
  retained: vmul x,[c] stored straight back to where x came from (x *= c, c >= 0.5)
in functions of the range, and prints them grouped by function with the function's call count from
any SSU_COUNT run given in COUNTS (none by default). Constants 0.5 in the moved pattern are skipped
(midpoints, not time).
"""
import re
import struct
import sys
from collections import defaultdict

sys.path.insert(0, r'C:\Games\ssuvr-work\analysis\refute-c3')
from common import *  # noqa: E402,F401

c = ctx()
lo, hi = (int(sys.argv[1], 16), int(sys.argv[2], 16)) if len(sys.argv) > 2 else (0x1f0000, 0x360000)
RIP = re.compile(r'rip \+ (0x[0-9a-f]+)\]')
found = defaultdict(list)
for fs, fe in c.funcs:
    if not (lo <= fs < hi) or fe - fs > 200000:
        continue
    ins = list(c.md.disasm(c.elf.read(fs, fe - fs), fs))
    for i, x in enumerate(ins):
        if not x.mnemonic.startswith('vmulss'):
            continue
        m = RIP.search(x.op_str)
        t = None
        if m:
            t = x.address + x.size + int(m.group(1), 16)
        else:
            # constant loaded into a register shortly before: vmovss xmmK, [rip + c]; vmulss d, a, xmmK
            regs_used = re.findall(r'xmm\d+', x.op_str)[1:]
            for p in reversed(ins[max(0, i - 6):i]):
                mm = RIP.search(p.op_str)
                if p.mnemonic == 'vmovss' and mm and p.op_str.split(',')[0] in regs_used:
                    t = p.address + p.size + int(mm.group(1), 16)
                    break
        if t is None:
            continue
        if not (0x590000 <= t < 0x640000):
            continue
        v = struct.unpack('<f', c.elf.read(t, 4))[0]
        if not (0 < v < 1) or v == 0.5:
            continue
        dst = re.findall(r'xmm\d+', x.op_str)
        if not dst:
            continue
        d = dst[0]
        prev = ins[max(0, i - 3):i]
        nxt = ins[i + 1:i + 5]
        sub_before = any(p.mnemonic.startswith('vsubss') and p.op_str.startswith(d + ',') for p in prev)
        add_after = any(n.mnemonic.startswith('vaddss') and d in n.op_str for n in nxt)
        store_after = [n for n in nxt[:2] if n.mnemonic == 'vmovss' and n.op_str.startswith('dword ptr [r')
                       and n.op_str.endswith(d)]
        load_before = [p for p in prev if p.mnemonic == 'vmovss' and p.op_str.startswith(d + ', dword ptr [r')
                       and 'rip' not in p.op_str]
        kind = None
        if sub_before and add_after:
            kind = 'moved'
        elif store_after and load_before and v >= 0.5:
            src = load_before[-1].op_str.split(', ', 1)[1]
            dst_mem = store_after[0].op_str.split(', ')[0]
            if src == dst_mem:
                kind = 'retained'
        if kind:
            found[fs].append((x.address, kind, v, t))
total = 0
for fs in sorted(found):
    items = found[fs]
    total += len(items)
    print(f'sub_{fs:x}: ' + ', '.join(f'{a:#x} {k} {v:g} @{t:#x}' for a, k, v, t in items))
print(f'{total} sites in {len(found)} functions', file=sys.stderr)
