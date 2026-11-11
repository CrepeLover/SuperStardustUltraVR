"""Hard-coded integer frame counters (2026-11-07, variable frame rate work).

Usage: frame_counters.py [lo hi]   (code range, default 0x1f0000-0x520000, the game logic)
Finds memory ints counted once per call and compared with a constant:
  inc/add 1 [m] ... cmp [m], N   (or the value loaded into a register then compared)
  dec/sub 1 [m] ... with mov [m], N elsewhere in the function (a countdown)
N >= 4. Prints function, the counter's operand, N and the sites, for reading by hand: at another world
rate N frames last N/rate seconds.
"""
import re
import sys
from collections import defaultdict

sys.path.insert(0, r'C:\Games\ssuvr-work\analysis\refute-c3')
from common import *  # noqa: E402,F401

c = ctx()
lo, hi = (int(sys.argv[1], 16), int(sys.argv[2], 16)) if len(sys.argv) > 2 else (0x1f0000, 0x520000)
MEM = re.compile(r'dword ptr (\[r(?!ip|sp|bp)\w+ \+ 0x[0-9a-f]+\])')
out = []
for fs, fe in c.funcs:
    if not (lo <= fs < hi) or fe - fs > 200000:
        continue
    ins = list(c.md.disasm(c.elf.read(fs, fe - fs), fs))
    incs, decs, sets, cmps = defaultdict(list), defaultdict(list), defaultdict(list), defaultdict(list)
    for i, x in enumerate(ins):
        m = MEM.search(x.op_str)
        if not m:
            continue
        mem = '+' + m.group(1).split('+ ')[1].rstrip(']')  # by offset: the base register differs between sites
        op = x.op_str
        if x.mnemonic == 'inc' or (x.mnemonic == 'add' and op.endswith(', 1')):
            incs[mem].append(x.address)
        elif x.mnemonic == 'dec' or (x.mnemonic == 'sub' and op.endswith(', 1')):
            decs[mem].append(x.address)
        elif x.mnemonic == 'mov' and op.startswith('dword ptr') and re.search(r', (0x[0-9a-f]+|\d+)$', op):
            sets[mem].append((x.address, int(op.rsplit(', ', 1)[1], 0)))
        elif x.mnemonic == 'cmp' and op.startswith('dword ptr') and re.search(r', (0x[0-9a-f]+|\d+)$', op):
            cmps[mem].append((x.address, int(op.rsplit(', ', 1)[1], 0)))
        elif x.mnemonic == 'mov' and re.match(r'e\w\w, dword ptr', op):
            reg = op.split(',')[0]
            for y in ins[i + 1:i + 4]:
                if y.mnemonic == 'cmp' and y.op_str.startswith(reg + ', ') and re.search(r', (0x[0-9a-f]+|\d+)$', y.op_str):
                    cmps[mem].append((y.address, int(y.op_str.rsplit(', ', 1)[1], 0)))
                    break
    for mem in incs:
        ns = [(a, n) for a, n in cmps.get(mem, []) if 4 <= n < 100000]
        if ns:
            out.append((fs, 'up', mem, incs[mem], ns))
    for mem in decs:
        ns = [(a, n) for a, n in sets.get(mem, []) if 4 <= n < 100000]
        if ns:
            out.append((fs, 'down', mem, decs[mem], ns))
for fs, kind, mem, at, ns in out:
    print(f'sub_{fs:x} {kind:4s} {mem:22s} N={",".join(str(n) for _, n in ns):12s} '
          f'count@{",".join(hex(a) for a in at[:3])} n@{",".join(hex(a) for a, _ in ns[:3])}')
print(f'{len(out)} counters', file=sys.stderr)
