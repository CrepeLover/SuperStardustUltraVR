"""Who stores the frame step / minimum frame time (floats like 1/60, 1/120, 1/90, 1/30) into fields +0xc / +0x14,
and every immediate store of those float bit patterns anywhere."""
import sys, struct, io, collections
sys.path.insert(0, r'C:\Users\<user>\AppData\Local\Temp\claude\C--Users-<user>\ac2a6782-da34-4ad3-a819-19f4f42a87ac\scratchpad\refute-c3')
from common import *

c = ctx()
vals = {struct.unpack('<I', struct.pack('<f', 1.0 / x))[0]: '1/%d' % x for x in (30, 60, 90, 120)}
vals.update({struct.unpack('<I', struct.pack('<f', float(x)))[0]: '%d.0' % x for x in (30, 60, 90, 120)})
hits = collections.defaultdict(list)
for fs, fe in c.funcs:
    if fe - fs > 200000:
        continue
    for ins in c.md.disasm(c.elf.read(fs, fe - fs), fs):
        if ins.mnemonic != 'mov' or not ins.op_str.startswith('dword ptr ['):
            continue
        try:
            imm = int(ins.op_str.rsplit(',', 1)[1].strip(), 16)
        except ValueError:
            continue
        if imm in vals:
            hits[vals[imm]].append((ins.address, ins.op_str, fs))
for k in sorted(hits):
    print('%s stored as an immediate %d times:' % (k, len(hits[k])))
    for a, o, fs in hits[k][:30]:
        print('    %#x sub_%x  mov %s' % (a, fs, o))
# also rodata 1/60 and 1/120 constants loaded and then stored to +0xc/+0x14
print()
for want in ('1/60', '1/120', '1/90'):
    bits = [b for b, n in vals.items() if n == want][0]
    for addr, name in ((a, n) for a, n in [(x, want) for x in c.find_rodata_u32(bits)] if hasattr(c, 'find_rodata_u32')):
        pass
