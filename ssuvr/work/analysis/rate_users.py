import sys, io, re, struct
sys.path.insert(0, r'C:\Users\<user>\AppData\Local\Temp\claude\C--Users-<user>\ac2a6782-da34-4ad3-a819-19f4f42a87ac\scratchpad\refute-c3')
from common import *
c = ctx()
rip = re.compile(r'\[rip \+ (0x[0-9a-f]+)\]')
# the 120.0 loads sixty_users.py reported
for a in (0x278739, 0x27874b, 0x3abc8f, 0x64a55):
    ins = list(c.md.disasm(c.elf.read(a, 16), a))[0]
    m = rip.search(ins.op_str)
    t = ins.address + ins.size + int(m.group(1), 16) if m else None
    v = struct.unpack('<f', c.elf.read(t, 4))[0] if t else None
    print('%#x: %s %s -> target %s value %s' % (a, ins.mnemonic, ins.op_str, hex(t) if t else None, v))
print()
# what begin-frame does with the update rate (vfunc10 result stored at [rbp-0xdc])
f = c.func_of(0x558ec1)
buf = io.StringIO()
c.disasm(f[0], f[1], mark={0x558ec1}, out=buf)
lines = buf.getvalue().splitlines()
idx = [i for i, l in enumerate(lines) if '0x558ec1:' in l][0]
print('\n'.join(l[:170] for l in lines[idx - 4: idx + 30]))
uses = [l for l in lines if 'rbp - 0xdc]' in l]
print('\nuses of [rbp-0xdc] in sub_%x:' % f[0])
print('\n'.join(l[:170] for l in uses))
