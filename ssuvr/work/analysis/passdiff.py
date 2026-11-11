"""Pass-log windows of a run (SHADPS4_LOG_PASSES with SHADPS4_LOG_PASSES_SHADERS=1): print each window's
time and frame count, then the draw kinds of the chosen windows that no earlier window had.
usage: passdiff.py <shad_log.txt> [first window index to diff, default: list only] [last index]"""
import re
import sys

log = open(sys.argv[1], encoding='utf-8', errors='replace').read().splitlines()
windows = []  # (line no, header, {kind: (calls, indices)})
for no, line in enumerate(log):
    if 'Passes in the last' in line:
        windows.append((no, line[line.index('Passes'):], {}))
    elif windows and 'Note:' in line and ' indices: ' in line:
        m = re.search(r'Note:\s+(\d+) x\s+(\d+) indices: (.*)$', line)
        if m:
            kind = re.sub(r' @0x[0-9a-f]+', '', m.group(3))
            kind = kind.replace('R8G8B8A8Unorm', 'RGBA8').replace('D32SfloatS8Uint', 'D32S8')
            kind = re.sub(r', scissor [0-9,x ]+', '', kind)
            windows[-1][2][kind] = (int(m.group(1)), int(m.group(2)))
for i, (no, header, kinds) in enumerate(windows):
    resolves = sum(c for k, (c, _) in kinds.items() if 'resolve' in k)
    print(f'{i:3} line {no:6} {header[:40]} {len(kinds):4} kinds, {resolves:4} resolves')
if len(sys.argv) > 2:
    first = int(sys.argv[2])
    last = int(sys.argv[3]) if len(sys.argv) > 3 else first
    seen = set()
    for _, _, kinds in windows[:first]:
        seen.update(kinds)
    new = {}
    for _, _, kinds in windows[first:last + 1]:
        for k, (c, n) in kinds.items():
            if k not in seen:
                pc, pn = new.get(k, (0, 0))
                new[k] = (pc + c, pn + n)
    print(f'--- kinds in windows {first}-{last} not seen before ({len(new)}):')
    for k, (c, n) in sorted(new.items(), key=lambda kv: -kv[1][0]):
        print(f'{c:7} x {n:10}  {k[:330]}')
