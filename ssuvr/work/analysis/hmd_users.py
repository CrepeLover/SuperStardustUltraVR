"""Callers of the Hmd / VrTracker imports, and float reads of +0x210 / +0x214 (STEREO_SEPARATION /
BG_STEREO_SEPARATION in the HMD config object) anywhere in .text."""
import sys, re
sys.path.insert(0, r'C:\Games\ssuvr-work\analysis\refute-c3')
from common import *
c = ctx()
for stub, label in sorted(c.stubs.items(), key=lambda x: x[1]):
    if 'Hmd' in label or 'VrTracker' in label:
        callers = sorted(set(c.func_of(x)[0] for x in c.calls.get(stub, []) if c.func_of(x)))
        print('%-45s %s' % (label, ' '.join('sub_%x' % f for f in callers)))
text = c.text
start, end = text['va'], text['va'] + text['filesz']
addr = start
hits = []
while addr < end:
    chunk = c.elf.read(addr, min(0x10000, end - addr))
    last = addr
    for x in c.md.disasm(chunk, addr):
        last = x.address + x.size
        if re.search(r'dword ptr \[\w+ \+ 0x21[04]\]', x.op_str) and x.mnemonic.startswith('v') and 'ss' in x.mnemonic:
            hits.append(x)
    addr = last if last > addr else addr + 1
print(len(hits), 'float reads of +0x210/+0x214')
for x in hits:
    f = c.func_of(x.address)
    print('  %#x sub_%x  %s %s' % (x.address, f[0] if f else 0, x.mnemonic, x.op_str))
