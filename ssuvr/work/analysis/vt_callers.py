"""Find the vtable holding sub_558700 / sub_559000 and the functions that call those slots."""
import sys, struct, collections, re
sys.path.insert(0, r'C:\Users\<user>\AppData\Local\Temp\claude\C--Users-<user>\ac2a6782-da34-4ad3-a819-19f4f42a87ac\scratchpad\refute-c3')
from common import *

c = ctx()
# dump the table around the two slots
base = 0x70fb80
data = c.elf.read(base, 0x140)
print('table around 0x70fc40:')
for i in range(0, len(data), 8):
    v = struct.unpack_from('<Q', data, i)[0]
    f = c.func_of(v) if 0 < v < 0x570000 else None
    print('  %#x: %#x %s' % (base + i, v, ('sub_%x' % f[0]) if f and f[0] == v else ''))

# vtable start: walk back from 0x70fc40 while entries are function starts
start = 0x70fc40
while True:
    v = struct.unpack('<Q', c.elf.read(start - 8, 8))[0]
    f = c.func_of(v) if 0 < v < 0x570000 else None
    if not f or f[0] != v:
        break
    start -= 8
print('vtable starts at %#x; begin slot +%#x, end slot +%#x' % (start, 0x70fc40 - start, 0x70fc48 - start))
off_begin, off_end = 0x70fc40 - start, 0x70fc48 - start

# who loads the vtable address (constructors)
print('references to the vtable start:', ['%#x in sub_%x' % (s, c.func_of(s)[0]) for s in sorted(c.riprefs.get(start, []))][:8])

# functions with indirect calls through both slots
pat_b = re.compile(r'call qword ptr \[r\w+ \+ %s\]' % hex(off_begin))
pat_e = re.compile(r'call qword ptr \[r\w+ \+ %s\]' % hex(off_end))
hits = collections.defaultdict(lambda: [0, 0])
for fs, fe in c.funcs:
    if fe - fs > 200000:
        continue
    for ins in c.md.disasm(c.elf.read(fs, fe - fs), fs):
        if ins.mnemonic != 'call':
            continue
        s = 'call ' + ins.op_str
        if pat_b.search(s):
            hits[fs][0] += 1
        elif pat_e.search(s):
            hits[fs][1] += 1
both = [(fs, b, e) for fs, (b, e) in hits.items() if b and e]
print('functions calling both slots:', ['sub_%x (%d, %d)' % x for x in sorted(both)][:20])
print('functions calling the begin slot only: %d, end slot only: %d' % (
    sum(1 for v in hits.values() if v[0] and not v[1]), sum(1 for v in hits.values() if v[1] and not v[0])))
