"""Print the SHADPS4_LOG_PASSES blocks of a shad_log.txt compactly.
usage: python -I blocks.py <shad_log.txt> [top-N per block] [first block] [last block]"""
import re, sys
path = sys.argv[1]
top = int(sys.argv[2]) if len(sys.argv) > 2 else 12
first = int(sys.argv[3]) if len(sys.argv) > 3 else 0
last = int(sys.argv[4]) if len(sys.argv) > 4 else 10**9
blocks = []
for line in open(path, encoding='utf-8', errors='replace'):
    m = re.search(r'^\[([^\]]*)\].*Note: Passes in the last ([\d.]+) s \((\d+) kinds', line)
    if m:
        blocks.append([m.group(1), m.group(3), []])
        continue
    m = re.search(r'Note:\s+(\d+) x\s+(\d+) indices: (.*)$', line)
    if m and blocks:
        blocks[-1][2].append((int(m.group(1)), int(m.group(2)), m.group(3)))
for i, (ts, kinds, rows) in enumerate(blocks):
    if i < first or i > last:
        continue
    print(f'=== block {i}  {ts}  {kinds} kinds')
    for calls, idx, key in rows[:top]:
        key = key.replace('R8G8B8A8', 'RGBA8').replace('D32SfloatS8Uint', 'D32S8').replace('viewport ', 'vp ').replace('scissor ', 'sc ')
        print(f'{calls:>6} {idx:>9}  {key}')
