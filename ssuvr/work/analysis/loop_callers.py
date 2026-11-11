"""Who calls the begin-frame (sub_558700) and end-frame (sub_559000) routines, transitively up a few levels;
also the callers of the 1/60 and 1/120 constants' users."""
import sys, collections
sys.path.insert(0, r'C:\Users\<user>\AppData\Local\Temp\claude\C--Users-<user>\ac2a6782-da34-4ad3-a819-19f4f42a87ac\scratchpad\refute-c3')
from common import *

c = ctx()

def callers(fn):
    out = collections.OrderedDict()
    for s in sorted(c.calls.get(fn, [])):
        f = c.func_of(s)
        out.setdefault(f[0] if f else 0, []).append(s)
    # also pointers to the function (vtables / callbacks)
    ptrs = c.relptr.get(fn, [])
    return out, ptrs

for root in (0x558700, 0x559000):
    print('== sub_%x' % root)
    frontier = [(root, 0)]
    seen = set()
    while frontier:
        fn, depth = frontier.pop(0)
        if fn in seen or depth > 3:
            continue
        seen.add(fn)
        cs, ptrs = callers(fn)
        f = c.func_of(fn)
        print('%s sub_%x (%d bytes): called from %s%s' % ('  ' * depth, fn, (f[1] - f[0]) if f else 0,
              ', '.join('sub_%x@%s' % (k, '/'.join('%#x' % x for x in v)) for k, v in cs.items()) or 'nothing direct',
              ('; pointer at ' + ' '.join('%#x' % p for p in ptrs[:4])) if ptrs else ''))
        for k in cs:
            frontier.append((k, depth + 1))
