"""Finds code that reads the HMD.config globe fields: loads [X + 0x420] (the config object, set by the
HMD config loader sub_25b710 at 0x25b96a) and then reads +0x230..+0x24c from it. Linear sweep of .text."""
import sys, re
sys.path.insert(0, r'C:\Games\ssuvr-work\analysis\refute-c3')
from common import *
c = ctx()
FIELDS = {0x230: 'PLANET_RADIUS', 0x234: 'FLY_RADIUS', 0x238: 'CAMERA_RADIUS', 0x23c: 'CAMERA_RADIUS_FAR',
          0x240: 'ROCK_SPAWN_RADIUS', 0x244: 'INSIDE_PLANET_SPAWN_RADIUS', 0x248: 'MENU_DEFAULT_CAMERA_DISTANCE',
          0x24c: 'MENU_SCALE', 0x218: 'MENU_OFFSET_SCALE', 0x210: 'STEREO_SEPARATION', 0x214: 'BG_STEREO_SEPARATION',
          0x250: 'USE_MOVEME'}
text = [p for p in c.elf.phdrs if p['type'] == PT_LOAD and p['flags'] & 1][0]
start, end = text['va'], text['va'] + text['filesz']
hits = []
recent = []  # (address, register loaded from +0x420)
addr = start
while addr < end:
    chunk = c.elf.read(addr, min(0x10000, end - addr))
    last = addr
    for x in c.md.disasm(chunk, addr):
        last = x.address + x.size
        m = re.match(r'(\w+), qword ptr \[\w+ \+ 0x420\]$', x.op_str)
        if x.mnemonic == 'mov' and m:
            recent.append((x.address, m.group(1)))
        recent = [r for r in recent if x.address - r[0] < 120]
        for reg_addr, reg in recent:
            m2 = re.search(r'\[' + reg + r' \+ (0x[0-9a-f]+)\]', x.op_str)
            if m2 and int(m2.group(1), 16) in FIELDS and x.address != reg_addr:
                hits.append((x.address, FIELDS[int(m2.group(1), 16)], x.mnemonic, x.op_str))
    addr = last if last > addr else addr + 1
for h in hits:
    f = c.func_of(h[0])
    print(f'{h[0]:#x} sub_{f[0]:x}  {h[1]:30s} {h[2]} {h[3]}' if f else f'{h[0]:#x} ?  {h[1]} {h[2]} {h[3]}')
