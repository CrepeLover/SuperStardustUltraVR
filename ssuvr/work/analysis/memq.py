"""Does SSU VR size anything from Pro mode or from the memory it is given?
1. call sites of memory-size queries and of sceKernelIsNeoMode,
2. the function that allocates the Shared System / Shared Video heaps (contains 0x555bfa)."""
import sys, io
sys.path.insert(0, r'C:\Users\<user>\AppData\Local\Temp\claude\C--Users-<user>\ac2a6782-da34-4ad3-a819-19f4f42a87ac\scratchpad\refute-c3')
from common import *

c = ctx()
names = ['sceKernelIsNeoMode', 'sceKernelGetDirectMemorySize', 'sceKernelAvailableDirectMemorySize',
         'sceKernelAvailableFlexibleMemorySize', 'sceKernelGetDirectMemoryType', 'sceKernelAllocateDirectMemory',
         'sceKernelAllocateMainDirectMemory', 'sceKernelMapDirectMemory', 'sceKernelMapNamedDirectMemory',
         'sceKernelMapFlexibleMemory', 'sceKernelMapNamedFlexibleMemory', 'sceKernelReserveVirtualRange',
         'sceKernelGetCompiledSdkVersion', 'sceKernelHasNeoMode']
for w in names:
    sites = []
    for st in c.stub_by_label.get(w, []):
        sites += c.calls.get(st, [])
    print('%-40s %d call sites: %s' % (w, len(sites), ', '.join('%#x(sub_%x)' % (s, c.func_of(s)[0]) for s in sorted(sites))[:300]))

mode = sys.argv[1] if len(sys.argv) > 1 else 'heap'
if mode == 'heap':
    f = c.func_of(0x555bfa)
    print('\n; heap function sub_%x .. %#x, callers %s' % (f[0], f[1], [hex(x) for x in sorted(c.calls.get(f[0], []))]))
    c.disasm(f[0], f[1], mark={0x555bfa, 0x555c2b})
elif mode == 'neo':
    for w in ('sceKernelIsNeoMode',):
        for st in c.stub_by_label.get(w, []):
            for s in sorted(c.calls.get(st, [])):
                f = c.func_of(s)
                print('\n; site %#x in sub_%x (callers %s)' % (s, f[0], [hex(x) for x in sorted(c.calls.get(f[0], []))][:6]))
                c.disasm(max(f[0], s - 0x40), s + 0x80, mark={s})
