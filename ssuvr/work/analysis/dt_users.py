"""Where the frame timing object at [0xcdf378] is used, which of its fields are read, and sub_12900."""
import sys, collections, io
sys.path.insert(0, r'C:\Users\<user>\AppData\Local\Temp\claude\C--Users-<user>\ac2a6782-da34-4ad3-a819-19f4f42a87ac\scratchpad\refute-c3')
from common import *

c = ctx()
buf = io.StringIO()
c.disasm(0x12900, 0x12990, out=buf)
print(buf.getvalue())

GLOBAL = 0xcdf378
sites = sorted(c.riprefs.get(GLOBAL, []))
print('%d instructions reference [%#x]' % (len(sites), GLOBAL))
field_reads = collections.defaultdict(list)
for s in sites:
    f = c.func_of(s)
    if not f:
        continue
    insns = list(c.md.disasm(c.elf.read(s, min(0x80, f[1] - s)), s))
    if not insns or insns[0].mnemonic != 'mov':
        continue
    reg = insns[0].op_str.split(',')[0].strip()
    for ins in insns[1:12]:
        ops = ins.op_str
        if ('[%s + ' % reg) in ops or ('[%s]' % reg) in ops:
            # field offset
            part = ops.split('[%s' % reg, 1)[1]
            off = 0
            if part.startswith(' + '):
                try:
                    off = int(part[3:].split(']')[0], 16)
                except ValueError:
                    continue
            field_reads[off].append((s, ins.address, ins.mnemonic, ops, f[0]))
        if ops.startswith(reg + ',') and ins.mnemonic in ('mov', 'lea', 'xor'):
            break
for off in sorted(field_reads):
    lst = field_reads[off]
    funcs = sorted(set('sub_%x' % x[4] for x in lst))
    print('field +%#x: %d uses in %d functions: %s' % (off, len(lst), len(funcs), ' '.join(funcs[:12])))
    for s, a, m, o, fs in lst[:4]:
        print('     %#x %s %s' % (a, m, o))
