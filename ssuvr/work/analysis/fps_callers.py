"""Who calls the timing / waiting / flipping imports, and where do 1/60 s constants live (SSU VR eboot 1.03)."""
import sys, collections, struct
sys.path.insert(0, r'C:\Users\<user>\AppData\Local\Temp\claude\C--Users-<user>\ac2a6782-da34-4ad3-a819-19f4f42a87ac\scratchpad\refute-c3')
from common import *

c = ctx()
wanted = ['sceKernelUsleep', 'sceKernelNanosleep', 'sceKernelSleep', 'sceKernelClockGettime', 'posix_gettimeofday',
          'sceKernelWaitEqueue', 'sceVideoOutGetFlipStatus', 'sceVideoOutSubmitFlip', 'sceGnmSubmitAndFlipCommandBuffers',
          'sceGnmSubmitAndFlipCommandBuffersForWorkload', 'sceHmdReprojectionSetUserEventStart',
          'sceHmdReprojectionSetUserEventEnd', 'sceVideoOutAddFlipEvent', 'posix_pthread_yield', 'sceKernelWaitSema',
          'sceVrTrackerGetTime', 'sceVideoOutSetFlipRate', 'sceKernelAddUserEvent', 'sceHmdReprojectionStart',
          'sceHmdReprojectionSetDisplayBuffers', 'sceGnmSubmitDone', 'sceRtcGetTime_t']
for w in wanted:
    stubs = c.stub_by_label.get(w, [])
    sites = []
    for st in stubs:
        sites += c.calls.get(st, [])
    by_func = collections.defaultdict(list)
    for s in sites:
        f = c.func_of(s)
        by_func[f if f else (0, 0)].append(s)
    print('%-46s stubs %s  %d call sites in %d functions' % (w, ' '.join('%#x' % s for s in stubs), len(sites), len(by_func)))
    for (fs, fe), ss in sorted(by_func.items()):
        print('      sub_%x (%d bytes): %s' % (fs, fe - fs, ' '.join('%#x' % x for x in sorted(ss))))

# Constants for a 60 Hz (and 120 Hz) frame: floats, doubles and integer micro/nanoseconds in the read-only data,
# with the instructions that load them rip-relative.
print()
consts = {
    'f32 1/60': struct.pack('<f', 1 / 60), 'f32 60': struct.pack('<f', 60.0), 'f32 1/120': struct.pack('<f', 1 / 120),
    'f32 120': struct.pack('<f', 120.0), 'f32 1/30': struct.pack('<f', 1 / 30), 'f32 16.6667ms': struct.pack('<f', 1000 / 60),
    'f64 1/60': struct.pack('<d', 1 / 60), 'f64 60': struct.pack('<d', 60.0), 'f64 1/120': struct.pack('<d', 1 / 120),
    'f64 16.6667ms': struct.pack('<d', 1000 / 60), 'f64 16666.67us': struct.pack('<d', 1e6 / 60),
}
for name, pat in consts.items():
    hits = []
    for seg in c.elf.phdrs:
        if seg['type'] != PT_LOAD or seg['filesz'] == 0:
            continue
        data = c.elf.d[seg['off']:seg['off'] + seg['filesz']]
        i = 0
        while True:
            i = data.find(pat, i)
            if i < 0:
                break
            if i % len(pat) == 0 or len(pat) == 4:
                hits.append(seg['va'] + i)
            i += 1
    refd = [(h, c.riprefs.get(h, [])) for h in hits]
    used = [(h, r) for h, r in refd if r]
    print('%-14s %d copies, %d referenced' % (name, len(hits), len(used)))
    for h, r in used[:12]:
        funcs = sorted(set('sub_%x' % c.func_of(x)[0] for x in r if c.func_of(x)))
        print('      %#x <- %d refs in %s' % (h, len(r), ' '.join(funcs[:10]) + (' ...' if len(funcs) > 10 else '')))
