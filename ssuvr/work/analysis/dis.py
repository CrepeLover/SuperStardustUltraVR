"""Annotated disassembly of whole functions: python -I dis.py <outdir> <func_va_hex> ..."""
import sys, os
sys.path.insert(0, r'C:\Users\<user>\AppData\Local\Temp\claude\C--Users-<user>\ac2a6782-da34-4ad3-a819-19f4f42a87ac\scratchpad\refute-c3')
from common import *

c = ctx()
outdir = sys.argv[1]
os.makedirs(outdir, exist_ok=True)
for a in sys.argv[2:]:
    va = int(a, 16)
    f = c.func_of(va)
    if not f:
        print('no function at', a)
        continue
    path = os.path.join(outdir, 'func_%x.txt' % f[0])
    with open(path, 'w', encoding='utf-8') as out:
        out.write('; sub_%x .. %#x (%d bytes)\n' % (f[0], f[1], f[1] - f[0]))
        c.disasm(f[0], f[1], out=out)
    print(path)
