"""Registered debug values applied once per world update as an increment, or as a multiplier stored
back far from its load (2026-11-08; the carrier bomb fall-off 0.99 / 0.005 escaped tunable_patterns.py).

Usage: tunable_increments.py [lo hi]   (code range, default 0x1f0000-0x560000)
For every vaddss / vsubss / vmulss whose operand is a registered value (tunables_all.tsv), directly or
through a register loaded from it a few instructions before: reports it when the result is stored to
memory within 6 instructions and the window (12 before, 6 after) shows no time step use (the bullet
update's dt at [rbp - 0x29c], or a vmulss by a register that holds a value read from a dt-like slot is
not detected - review by hand). Prints name, default, value in the rescale table or not, site, the
instruction and the store, so the per-update ones can be added to tunable_table.py.
"""
import csv
import re
import struct
import sys

sys.path.insert(0, r'C:\Games\ssuvr-work\analysis\refute-c3')
from common import *  # noqa: E402,F401

c = ctx()
lo, hi = (int(sys.argv[1], 16), int(sys.argv[2], 16)) if len(sys.argv) > 2 else (0x1f0000, 0x560000)
rows = list(csv.DictReader(open(r'C:\Games\ssuvr-work\analysis\tunables_all.tsv'), delimiter='\t'))
name_at = {}
for r in rows:
    if r['address']:
        name_at[int(r['address'], 16)] = (r['name'], r['default'])
cpp = open(r'C:\Games\Farpoint-src\src\core\libraries\kernel\wait_stats.cpp', encoding='utf-8').read()
in_table = set(int(a, 16) for a in re.findall(r'\{(0x9[0-9a-f]+), Rescale::', cpp))
RIP = re.compile(r'rip \+ (0x[0-9a-f]+)\]')
DT = re.compile(r'rbp - 0x29c\]')

for fs, fe in c.funcs:
    if not (lo <= fs < hi) or fe - fs > 200000:
        continue
    ins = list(c.md.disasm(c.elf.read(fs, fe - fs), fs))
    for i, x in enumerate(ins):
        if x.mnemonic not in ('vaddss', 'vsubss', 'vmulss'):
            continue
        t = None
        m = RIP.search(x.op_str)
        if m:
            t = x.address + x.size + int(m.group(1), 16)
        else:
            srcs = re.findall(r'xmm\d+', x.op_str)[1:]
            for p in reversed(ins[max(0, i - 8):i]):
                mm = RIP.search(p.op_str)
                if p.mnemonic == 'vmovss' and mm and p.op_str.split(',')[0] in srcs:
                    t = p.address + p.size + int(mm.group(1), 16)
                    break
        if t not in name_at:
            continue
        name, default = name_at[t]
        d = re.findall(r'xmm\d+', x.op_str)[0]
        after = ins[i + 1:i + 7]
        store = next((n for n in after if n.mnemonic == 'vmovss' and n.op_str.startswith('dword ptr [r')
                      and n.op_str.endswith(', ' + d) and 'rip' not in n.op_str), None)
        if store is None:
            continue
        window = ins[max(0, i - 12):i + 7]
        if any(DT.search(w.op_str) for w in window):
            continue
        mem = store.op_str.split(', ')[0]
        loaded = any(p.op_str.endswith(mem) or (mem in p.op_str) for p in ins[max(0, i - 12):i])
        print(f'{"T" if t in in_table else "-"} {x.mnemonic:6s} {name[5:]:70s} {default:>8s} sub_{fs:x} {x.address:#x} '
              f'| {x.op_str} -> {store.op_str}{"  (store-back)" if loaded else ""}')
