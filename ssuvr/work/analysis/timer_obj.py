import sys, io, re, struct
sys.path.insert(0, r'C:\Users\<user>\AppData\Local\Temp\claude\C--Users-<user>\ac2a6782-da34-4ad3-a819-19f4f42a87ac\scratchpad\refute-c3')
from common import *
c = ctx()
G = 0x9b6250
refs = sorted(c.riprefs.get(G, []))
print('%d references to [%#x]' % (len(refs), G))
for r in refs:
    f = c.func_of(r)
    ins = list(c.md.disasm(c.elf.read(r, 16), r))[0]
    line = '%#x sub_%x  %s %s' % (r, f[0] if f else 0, ins.mnemonic, ins.op_str)
    # follow a few instructions for the virtual call slot and xmm0 setup
    tail = list(c.md.disasm(c.elf.read(r, 80), r))[:10]
    slot = [t for t in tail if t.mnemonic == 'call' or ('[rax + 0x' in t.op_str and t.mnemonic == 'mov')]
    print(line, ' | ', '; '.join('%s %s' % (t.mnemonic, t.op_str) for t in tail[1:7]))
