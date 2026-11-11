import sys
sys.path.insert(0, r'C:\Users\<user>\AppData\Local\Temp\claude\C--Users-<user>\ac2a6782-da34-4ad3-a819-19f4f42a87ac\scratchpad\refute-c3')
from common import *
c = ctx()
f = c.func_of(0x25c509)
# walk instructions from the function start so the listing is aligned, print a window around the Pro check
import io
buf = io.StringIO()
c.disasm(f[0], f[1], mark={0x25c509}, out=buf)
lines = buf.getvalue().splitlines()
idx = [i for i, l in enumerate(lines) if '0x25c509:' in l][0]
print('\n'.join(l[:165] for l in lines[idx - 22: idx + 40]))
