"""Lists the Lua debug values registered in a function: name, descriptor, value address (+0xc) and the
default constant (the rodata float clamped/loaded just before the store). usage: debug_values.py <func hex> [len hex]"""
import sys, re, struct
sys.path.insert(0, r'C:\Games\ssuvr-work\analysis\refute-c3')
from common import *
c = ctx()
start = int(sys.argv[1], 16); length = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0x1000
def cstr(va):
    b = c.elf.read(va, 96); return b[:b.find(b'\x00')].decode(errors='replace')
ins = list(c.md.disasm(c.elf.read(start, length), start))
name = None; consts = []; desc = None
for x in ins:
    m = re.search(r'rip \+ (0x[0-9a-f]+)', x.op_str)
    if not m:
        continue
    t = x.address + x.size + int(m.group(1), 16)
    if x.mnemonic == 'lea' and 0x5b0000 < t < 0x640000:
        s = cstr(t)
        if s.startswith('sd3') or s.startswith('game'):
            name = s; consts = []
    elif x.mnemonic == 'lea' and 0x980000 < t < 0xa00000:
        desc = t
    elif x.mnemonic in ('vmovss', 'vmaxss', 'vminss') and (0x5b0000 < t < 0x640000 or 0x700000 < t < 0x740000) and 'xmm0, dword' in x.op_str:
        consts.append(struct.unpack('<f', c.elf.read(t, 4))[0])
    elif x.mnemonic == 'vmovss' and x.op_str.startswith('dword ptr [rip') and name:
        print(f'{name:45s} value [{t:#x}]  default {("%g" % consts[-1]) if consts else "?":<8s}  (registered near {x.address:#x})')
        name = None
