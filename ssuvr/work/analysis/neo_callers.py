"""Callers of sceKernelIsNeoMode (and other quality/perf probes) in the SSU VR eboot, with context."""
import sys, collections, io
sys.path.insert(0, r'C:\Users\<user>\AppData\Local\Temp\claude\C--Users-<user>\ac2a6782-da34-4ad3-a819-19f4f42a87ac\scratchpad\refute-c3')
from common import *

c = ctx()
for w in ['sceKernelIsNeoMode', 'sceKernelGetCpuFrequency', 'sceGnmGetGpuCoreClockFrequency', 'sceKernelHasNeoMode',
          'sceKernelGetSystemSwVersion', 'sceGnmIsUserPaEnabled', 'sceGnmGetTheTessellationFactorRingBufferBaseAddress']:
    stubs = c.stub_by_label.get(w, [])
    sites = []
    for st in stubs:
        sites += c.calls.get(st, [])
    print('%-40s %d call sites' % (w, len(sites)))
    for s in sorted(sites):
        f = c.func_of(s)
        print('   site %#x in sub_%x' % (s, f[0] if f else 0))
        start = max(f[0], s - 0x30) if f else s - 0x30
        buf = io.StringIO()
        c.disasm(start, s + 0x60, mark={s}, out=buf)
        print(buf.getvalue())
