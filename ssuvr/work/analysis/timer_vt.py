import sys, io, re, struct
sys.path.insert(0, r'C:\Users\<user>\AppData\Local\Temp\claude\C--Users-<user>\ac2a6782-da34-4ad3-a819-19f4f42a87ac\scratchpad\refute-c3')
from common import *
c = ctx()
init = c.elf.read(0x5a7ef0, 16)
print('initial +8..+0x17:', ['%#010x=%g' % (v, struct.unpack('<f', struct.pack('<I', v))[0]) for v in struct.unpack('<4I', init)])
VT = 0x70dfe8
for i in range(10):
    v = struct.unpack('<Q', c.elf.read(VT + 8 * i, 8))[0]
    f = c.func_of(v) if 0 < v < 0x570000 else None
    if not f or f[0] != v:
        print('slot +%#x: %#x (end of table)' % (8 * i, v))
        break
    print('\n==== slot +%#x: sub_%x (%d bytes)' % (8 * i, v, f[1] - f[0]))
    c.disasm(f[0], min(f[1], f[0] + 0x120))
