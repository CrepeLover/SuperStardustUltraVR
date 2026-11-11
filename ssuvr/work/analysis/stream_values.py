"""Debug values set from the game's data (stream.dat): every 'sd3__...' / 'game__...' name matching a
pattern, with the float that follows it. usage: stream_values.py <regex>"""
import mmap
import re
import struct
import sys

pat = re.compile(rb'(?:sd3__|game__)[A-Za-z0-9_]*' + sys.argv[1].encode() + rb'[A-Za-z0-9_]*')
with open(r'C:\Games\SuperStardustUltraVR\games\CUSA05951\stream.dat', 'rb') as f:
    m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
    seen = {}
    for hit in pat.finditer(m):
        name = hit.group(0).decode()
        tail = m[hit.end():hit.end() + 16]
        # The float is the first 4-aligned word after the name's terminator that reads as a sane float.
        vals = []
        for off in range(1, 13):
            v = struct.unpack_from('<f', tail, off)[0] if off + 4 <= len(tail) else None
            vals.append(v)
        key = (name, hit.start() // (1 << 20))
        if name not in seen:
            seen[name] = (hit.start(), tail.hex(' '))
    for name, (pos, tail) in sorted(seen.items()):
        print(f'{pos:#012x} {name:55s} {tail}')
