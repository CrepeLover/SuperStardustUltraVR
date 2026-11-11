import sys, io, re
sys.path.insert(0, r'C:\Users\<user>\AppData\Local\Temp\claude\C--Users-<user>\ac2a6782-da34-4ad3-a819-19f4f42a87ac\scratchpad\refute-c3')
from common import *
c = ctx()
f = c.func_of(0x25ca3f)
print('; sub_%x .. %#x callers %s' % (f[0], f[1], [hex(x) for x in sorted(c.calls.get(f[0], []))]))
rip = re.compile(r'\[rip \+ (0x[0-9a-f]+)\]')
keys = []
for ins in c.md.disasm(c.elf.read(f[0], f[1] - f[0]), f[0]):
    m = rip.search(ins.op_str)
    if ins.mnemonic == 'lea' and m:
        t = ins.address + ins.size + int(m.group(1), 16)
        b = c.elf.read(t, 64)
        if b:
            s = b.split(b'\0')[0]
            if len(s) >= 3 and all(32 <= x < 127 for x in s):
                keys.append('%#x %s' % (ins.address, s.decode()))
print('strings:', '\n  '.join(keys))
buf = io.StringIO()
c.disasm(f[0], f[1], mark={0x25ca3f}, out=buf)
lines = buf.getvalue().splitlines()
idx = [i for i, l in enumerate(lines) if '0x25ca3f:' in l][0]
print('\n'.join(l[:170] for l in lines[idx - 45: idx + 45]))
