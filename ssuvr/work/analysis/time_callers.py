"""Callers of the game's clock / sleep helpers, and what else those callers call."""
import sys, collections
sys.path.insert(0, r'C:\Users\<user>\AppData\Local\Temp\claude\C--Users-<user>\ac2a6782-da34-4ad3-a819-19f4f42a87ac\scratchpad\refute-c3')
from common import *

c = ctx()
helpers = {0x4c030: 'clock_us A', 0x4c0c0: 'clock_us B', 0x12880: 'clock C', 0x4c150: 'usleep wrapper',
           0x1e7440: 'usleep wrapper 2', 0x1e7430: 'yield wrapper', 0x9670: 'waitsema A', 0x12e40: 'waitsema B',
           0x4c680: 'waitsema C'}
for h, label in helpers.items():
    sites = sorted(c.calls.get(h, []))
    funcs = collections.OrderedDict()
    for s in sites:
        f = c.func_of(s)
        funcs.setdefault(f[0] if f else 0, []).append(s)
    print('%s sub_%x: %d call sites in %d functions' % (label, h, len(sites), len(funcs)))
    for fs, ss in list(funcs.items())[:40]:
        f = c.func_of(fs)
        # imports this caller uses
        imps = set()
        if f:
            for ins in c.md.disasm(c.elf.read(f[0], f[1] - f[0]), f[0]):
                if ins.mnemonic == 'call' and ins.op_str.startswith('0x'):
                    t = int(ins.op_str, 16)
                    if t in c.stubs:
                        imps.add(c.stubs[t])
        print('    sub_%x (%d bytes) sites %s  imports: %s' % (fs, (f[1] - f[0]) if f else 0, ' '.join('%#x' % x for x in ss),
                                                          ', '.join(sorted(imps))[:200]))
