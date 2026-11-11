"""Every function that loads the frame timer pointer [0x9b6250], and what it does with it: the
virtual calls made on it (vtable 0x70dfe8: 0x10 get_rate, 0x18 set_rate, 0x20 get_step?,
0x28 set_cap, 0x30 accumulate) and direct reads/writes of its fields +8/+0xc/+0x10/+0x14.
Tracks the register the pointer lands in through a few instructions (simple, per function)."""
import io
import re
import sys

sys.path.insert(0, r'C:\Games\ssuvr-work\analysis\refute-c3')
from common import *  # noqa: E402,F403

c = ctx()
GLOBAL = 0x9b6250
FIELDS = {0x8: 'rate', 0xc: 'step', 0x10: 'cap_rate', 0x14: 'min_frame'}
sites = sorted(c.riprefs.get(GLOBAL, []))
print(f'{len(sites)} loads of [{GLOBAL:#x}]')
by_func = {}
for s in sites:
    f = c.func_of(s)
    by_func.setdefault(f, []).append(s)

for f, ss in sorted(by_func.items(), key=lambda kv: kv[0] or (0, 0)):
    if f is None:
        print('?? no function for', [hex(s) for s in ss])
        continue
    start, end = f
    ins = list(c.md.disasm(c.elf.read(start, end - start), start))
    notes = []
    for s in ss:
        k = next(i for i, x in enumerate(ins) if x.address == s)
        m = re.match(r'(\w+), qword ptr \[rip', ins[k].op_str)
        if not m:
            notes.append(f'{s:#x}: {ins[k].mnemonic} {ins[k].op_str} (not a plain load)')
            continue
        obj = m.group(1)
        vt = None
        for x in ins[k + 1:k + 14]:
            op = x.op_str
            mv = re.match(r'(\w+), qword ptr \[' + obj + r'\]$', op)
            if x.mnemonic == 'mov' and mv:
                vt = mv.group(1)
                continue
            mc = re.match(r'qword ptr \[(\w+) \+ (0x[0-9a-f]+)\]', op)
            if x.mnemonic == 'call' and mc and mc.group(1) == vt:
                notes.append(f'{x.address:#x}: vcall +{mc.group(2)}')
                break
            for off, name in FIELDS.items():
                if f'[{obj} + {off:#x}]' in op:
                    rw = 'WRITE' if op.startswith('dword ptr') else 'read'
                    notes.append(f'{x.address:#x}: {rw} {name} ({x.mnemonic} {op})')
            if x.mnemonic in ('ret', 'jmp') or (x.mnemonic == 'call' and not mc):
                break
            if re.match(obj + r',', op) and x.mnemonic in ('mov', 'lea', 'xor'):
                break
    print(f'sub_{start:x} ({end - start} bytes): ' + '; '.join(notes))
