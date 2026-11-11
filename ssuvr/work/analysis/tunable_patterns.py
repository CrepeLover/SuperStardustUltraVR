"""How each per-update-looking tunable is applied (2026-11-07, variable frame rate work).

Usage: tunable_patterns.py [name regex]
Reads tunables_all.tsv (tunables_all.py). For every reader site of the candidate values (fractions in
0..1 with lerp/damping/decay/slowdown/friction/mul-style names, frame counts, per-frame speeds), walks
forward from the site over a short straight-line window following the register that holds the value
(or the product it went into) and reports the arithmetic:
  moved    (to - from) * v + from          : a lerp; v is the fraction moved each update
  retained (x - to) * v + to, or x * v      : v is the fraction kept each update
  scaled   v meets another multiply first   : maybe time-scaled (dt * 60 ...) - read by hand
  other    anything else (compare, store, call)
and prints one line per site with the instructions it saw, so the classes can be checked by eye.
"""
import csv
import re
import sys

sys.path.insert(0, r'C:\Games\ssuvr-work\analysis\refute-c3')
from common import *  # noqa: E402,F401

c = ctx()
pat = re.compile(sys.argv[1]) if len(sys.argv) > 1 else None
NAME = re.compile(r'lerp|_mul$|per_frame|frames|damping|decay|friction|drag|slowdown|_mul_|smooth|iir|_factor', re.I)
SKIP = re.compile(r'fade_time|_time$|seconds|scale_|_scale|force|health|alpha|fade_value|spawn_factor|'
                  r'range_from|music|volume|text_scale|power_factor|Singularity|cap_fade', re.I)
rows = list(csv.DictReader(open(r'C:\Games\ssuvr-work\analysis\tunables_all.tsv'), delimiter='\t'))


def wanted(r):
    if not r['site']:
        return False
    if pat:
        return bool(pat.search(r['name']))
    try:
        v = float(r['default'])
    except ValueError:
        v = None
    n = r['name']
    if 'frames' in n.lower():
        return True
    return bool(NAME.search(n)) and not SKIP.search(n) and v is not None and 0 < v < 1


def short(x):
    op = re.sub(r'dword ptr |xmmword ptr |qword ptr ', '', x.op_str)
    op = re.sub(r'\[rip \+ 0x[0-9a-f]+\]', '[V?]', op)
    return f'{x.mnemonic} {op}'


def regs(op):
    return re.findall(r'\b[xy]mm\d+\b', op)


def analyse(site):
    fs, fe = c.func_of(site)
    ins = list(c.md.disasm(c.elf.read(site, min(0x120, fe - site)), site))
    first = ins[0]
    window = [short(first)]
    r0 = regs(first.op_str)
    if not r0:
        return 'other', window
    cur = r0[0]  # register holding the value (or its product)
    stage = 'load' if first.mnemonic in ('vmovss', 'vbroadcastss') else 'mul'
    if first.mnemonic.startswith('vmul'):
        stage = 'mul'
    elif first.mnemonic.startswith(('vsub', 'vadd', 'vucomiss', 'vcomiss', 'vmin', 'vmax')):
        return 'other', window
    elif first.mnemonic.startswith('mov') and not first.mnemonic.startswith('vmov'):
        return 'int', window
    muls = 1 if stage == 'mul' else 0
    for x in ins[1:16]:
        rr = regs(x.op_str)
        if cur not in rr:
            if rr and rr[0] == cur:
                break
            continue
        window.append(short(x))
        dst = rr[0] if not x.op_str.startswith(('dword', 'xmmword')) else None
        if x.mnemonic.startswith(('vmulss', 'vmulps')):
            muls += 1
            if dst:
                cur = dst
            if muls > 1:
                return 'scaled', window
        elif x.mnemonic.startswith(('vaddss', 'vaddps', 'vfmadd')):
            return ('moved' if muls == 1 else 'retained'), window
        elif x.mnemonic.startswith(('vsubss', 'vsubps')):
            return ('retained-sub' if muls == 1 else 'other'), window
        elif x.mnemonic in ('vmovss', 'vmovaps', 'vmovups') and dst:
            cur = dst
        elif dst is None:
            return ('retained' if muls == 1 else 'stored'), window
        elif x.mnemonic.startswith(('vucomiss', 'vcomiss', 'vmax', 'vmin', 'vcvt')):
            return 'other', window
        elif x.mnemonic in ('vshufps', 'vpshufd', 'vinsertps', 'vbroadcastss', 'vunpcklps', 'vmovsldup'):
            if dst:
                cur = dst
        if len(window) > 7:
            break
    return 'other', window


seen = set()
for r in rows:
    if not wanted(r):
        continue
    site = int(r['site'], 16)
    if site in seen:
        continue
    seen.add(site)
    cls, window = analyse(site)
    # what the multiply's other operand was: a lerp has a vsub just before it
    fs, fe = c.func_of(site)
    before = list(c.md.disasm(c.elf.read(max(fs, site - 0x18), site - max(fs, site - 0x18)), max(fs, site - 0x18)))
    prev = '; '.join(short(x) for x in before[-2:])
    name = r['name'].replace('sd3__', '')
    print(f'{cls:12s} {r["default"]:>6s} {name[:62]:62s} {r["reader"]} {r["site"]} | {prev} || ' + ' ; '.join(window))
