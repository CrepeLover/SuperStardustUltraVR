"""Find the dump files (user\\shader\\dumps, dump_shaders=true) of the stage hashes the pass log prints
(SHADPS4_LOG_PASSES_SHADERS=1): those are HashCombine(program hash, permutation index).
usage: shadermap.py <dumps dir> <logged hash hex>..."""
import os
import re
import sys

M = (1 << 64) - 1


def combine(seed, value):
    return (seed ^ ((value + 0x9E3779B9 + ((seed << 12) & M) + (seed >> 4)) & M)) & M


dumps = sys.argv[1]
wanted = {int(h, 16): h for h in sys.argv[2:]}
programs = {}
for name in os.listdir(dumps):
    m = re.match(r'([a-z]+)_0x([0-9a-f]{16})', name)
    if m:
        programs.setdefault((m.group(1), int(m.group(2), 16)), []).append(name)
for (stage, pgm), names in sorted(programs.items()):
    for perm in range(8):
        h = combine(pgm, perm)
        if h in wanted:
            print(f'{wanted[h]} = {stage} program {pgm:#018x} permutation {perm}:')
            for n in sorted(names):
                print('   ', n)
