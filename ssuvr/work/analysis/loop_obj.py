"""Callers of the game loop sub_426e10 (direct or through a vtable), with context, and stores to +0xc/+0x14 near
float loads in functions that also reference 1/60-ish constants."""
import sys, io, struct
sys.path.insert(0, r'C:\Users\<user>\AppData\Local\Temp\claude\C--Users-<user>\ac2a6782-da34-4ad3-a819-19f4f42a87ac\scratchpad\refute-c3')
from common import *

c = ctx()
LOOP = 0x426e10
print('direct callers:', ['%#x in sub_%x' % (s, c.func_of(s)[0]) for s in sorted(c.calls.get(LOOP, []))])
print('pointers to it:', ['%#x' % p for p in c.relptr.get(LOOP, [])][:8])
for s in sorted(c.calls.get(LOOP, [])):
    f = c.func_of(s)
    buf = io.StringIO()
    c.disasm(max(f[0], s - 0x90), s + 0x30, mark={s}, out=buf)
    print(buf.getvalue())

# The loop reads [this+8] -> obj; obj+0xc = step, obj+0x14 = minimum frame time. Find functions that store
# xmm registers to [reg+0xc] and [reg+0x14] within a few instructions of each other (an init of both fields).
cands = []
for fs, fe in c.funcs:
    if fe - fs > 100000:
        continue
    ins_list = list(c.md.disasm(c.elf.read(fs, fe - fs), fs))
    for i, ins in enumerate(ins_list):
        if ins.mnemonic in ('vmovss', 'movss', 'mov') and ins.op_str.startswith('dword ptr [') and '+ 0xc]' in ins.op_str.split(',')[0]:
            reg = ins.op_str.split('[')[1].split(' ')[0]
            for j in range(max(0, i - 8), min(len(ins_list), i + 8)):
                o = ins_list[j]
                if o.mnemonic in ('vmovss', 'movss', 'mov') and o.op_str.startswith('dword ptr [%s + 0x14]' % reg):
                    cands.append((fs, ins.address, ins.op_str, o.address, o.op_str))
print('%d candidate inits of both +0xc and +0x14' % len(cands))
for x in cands[:40]:
    print('  sub_%x  %#x %s | %#x %s' % x)
