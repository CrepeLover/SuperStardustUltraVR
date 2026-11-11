"""Instructions that read rodata floats 60.0 / 1/60 / 59.94 / 1/59.94 (and doubles), with the function and nearby string refs."""
import sys, struct, collections, re
sys.path.insert(0, r'C:\Users\<user>\AppData\Local\Temp\claude\C--Users-<user>\ac2a6782-da34-4ad3-a819-19f4f42a87ac\scratchpad\refute-c3')
from common import *

c = ctx()
want32 = {struct.unpack('<I', struct.pack('<f', v))[0]: n for v, n in
          ((60.0, '60'), (1 / 60.0, '1/60'), (59.94, '59.94'), (1 / 59.94, '1/59.94'), (0.016683, '0.016683'),
           (120.0, '120'), (1 / 120.0, '1/120'), (30.0, '30'), (1 / 30.0, '1/30'))}
want64 = {struct.unpack('<Q', struct.pack('<d', v))[0]: n + 'd' for v, n in
          ((60.0, '60'), (1 / 60.0, '1/60'), (59.94, '59.94'))}
rip = re.compile(r'\[rip \+ (0x[0-9a-f]+)\]')
by_func = collections.defaultdict(list)
for fs, fe in c.funcs:
    if fe - fs > 200000:
        continue
    for ins in c.md.disasm(c.elf.read(fs, fe - fs), fs):
        m = rip.search(ins.op_str)
        if not m:
            continue
        target = ins.address + ins.size + int(m.group(1), 16)
        if not (0x570000 <= target < 0x700000):
            continue
        try:
            raw = c.elf.read(target, 8)
        except Exception:
            continue
        if raw is None or len(raw) < 8:
            continue
        v32 = struct.unpack_from('<I', raw)[0]
        v64 = struct.unpack_from('<Q', raw)[0]
        name = None
        if 'sd' in ins.mnemonic and v64 in want64:
            name = want64[v64]
        elif v32 in want32 and 'sd' not in ins.mnemonic:
            name = want32[v32]
        if name:
            by_func[fs].append((ins.address, ins.mnemonic, name))


def strings_in(fs, fe, limit=4):
    out = []
    for ins in c.md.disasm(c.elf.read(fs, fe - fs), fs):
        m = rip.search(ins.op_str)
        if ins.mnemonic == 'lea' and m:
            t = ins.address + ins.size + int(m.group(1), 16)
            try:
                b = c.elf.read(t, 48)
            except Exception:
                continue
            if b is None:
                continue
            s = b.split(b'\0')[0]
            if len(s) >= 4 and all(32 <= x < 127 for x in s):
                out.append(s.decode())
        if len(out) >= limit:
            break
    return out


for fs in sorted(by_func):
    f = c.func_of(fs)
    uses = by_func[fs]
    print('sub_%x (%d bytes): %s | strings: %s' % (fs, f[1] - f[0], ', '.join('%s@%#x' % (n, a) for a, m, n in uses[:6]),
                                                  '; '.join(strings_in(f[0], f[1]))[:160]))
