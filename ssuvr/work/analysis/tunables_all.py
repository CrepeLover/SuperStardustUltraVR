"""Inventory of the game's registered Lua debug values (2026-11-07, for the variable frame rate work).

Usage: tunables_all.py [out.tsv]
Finds every function that registers sd3__* names (lea of the name string), lists name / value address /
default as debug_values.py does, then every reader of each value address (rip-relative, outside the
registration functions) with a rough classification of how the read value is used in that function:
  dt      the value meets the function's time-step argument (xmm0 at entry, tracked through registers
          and stack slots on a straight-line walk of the function, ignoring branches)
  nodt    it does not (in this walk): a per-update constant when the function runs once an update
  copy    it is only stored somewhere (to an object field / another slot) - follow up by hand
Writes a TSV (name, address, default, reader function, site, class) and prints a summary.
"""
import re
import struct
import sys
from collections import defaultdict

sys.path.insert(0, r'C:\Games\ssuvr-work\analysis\refute-c3')
from common import *  # noqa: E402,F401

c = ctx()
code = c.elf.read(0, 0x6e4e68)
RIP = re.compile(r'rip \+ (0x[0-9a-f]+)\]')


def target(x):
    m = RIP.search(x.op_str)
    return x.address + x.size + int(m.group(1), 16) if m else None


def cstr(va):
    b = c.elf.read(va, 96)
    return b[:b.find(b'\x00')].decode(errors='replace')


# 1. registration functions and their values
values = {}  # address -> (name, default)
reg_funcs = set()
for fs, fe in c.funcs:
    if fe - fs > 600000:
        continue
    ins = list(c.md.disasm(code[fs:fe], fs))
    name = None
    consts = []
    found = False
    for x in ins:
        t = target(x)
        if t is None:
            continue
        if x.mnemonic == 'lea' and 0x5b0000 < t < 0x640000:
            s = cstr(t)
            if s.startswith('sd3__'):
                name, consts, found = s, [], True
        elif x.mnemonic in ('vmovss', 'vmaxss', 'vminss') and 'xmm0, dword' in x.op_str and (
                0x5b0000 < t < 0x640000 or 0x700000 < t < 0x740000):
            consts.append(struct.unpack('<f', c.elf.read(t, 4))[0])
        elif name and x.op_str.startswith('dword ptr [rip') and x.mnemonic in ('vmovss', 'mov') and 0x980000 < t < 0xa00000:
            values[t] = (name, consts[-1] if consts else None)
            name = None
    if found:
        reg_funcs.add(fs)
print(f'{len(values)} values in {len(reg_funcs)} registration functions', file=sys.stderr)

# 2. readers
FLOAT_OPS = ('vmulss', 'vaddss', 'vsubss', 'vdivss', 'vmulps', 'vaddps', 'vsubps', 'vminss', 'vmaxss',
             'vfmadd', 'vfmsub', 'vfnmadd', 'vfnmsub', 'vinsertps', 'vshufps', 'vpshufd', 'vbroadcastss',
             'vunpcklps', 'vmovaps', 'vmovss', 'vmovups', 'vcvt', 'vsqrtss', 'vandps', 'vxorps', 'vblendvps')


def regs_of(op):
    return re.findall(r'\b([xy]mm\d+)\b', op)


def mem_of(op):
    m = re.search(r'(?:dword|qword|xmmword|ymmword) ptr (\[[^\]]+\])', op)
    return m.group(1) if m else None


def classify(fs, fe, site):
    """Straight-line tag walk: DT from entry xmm0, VAL from the load at site."""
    tags = defaultdict(set)  # register or memory text -> {'DT','VAL'}
    tags['xmm0'] = {'DT'}
    met = False
    stored = False
    used = False
    for x in c.md.disasm(code[fs:fe], fs):
        op = x.op_str
        if x.mnemonic == 'call':
            # caller-saved xmm registers are clobbered; xmm0 return
            for r in list(tags):
                if r.startswith('xmm') or r.startswith('ymm'):
                    if 'VAL' in tags[r] and x.address > site:
                        used = True  # passed to a call
                    del tags[r]
            continue
        if not (x.mnemonic.startswith('v') or x.mnemonic in ('mov', 'movss')):
            continue
        regs = regs_of(op)
        mem = mem_of(op)
        if x.address == site:
            if regs:
                tags[regs[0]] = {'VAL'}
            continue
        if not regs:
            continue
        dst = regs[0] if not op.startswith(('dword', 'qword', 'xmmword', 'ymmword')) else None
        srcs = set()
        src_regs = regs[1:] if dst else regs
        for r in src_regs:
            srcs |= tags.get(r, set())
        if mem and dst:
            srcs |= tags.get(mem, set())
        three_op = x.mnemonic.startswith(('vmul', 'vadd', 'vsub', 'vdiv', 'vfm', 'vfnm', 'vmin', 'vmax'))
        if x.mnemonic.startswith(('vfmadd', 'vfmsub', 'vfnmadd', 'vfnmsub')) and dst:
            srcs |= tags.get(dst, set())
        if dst is None:  # store to memory
            if 'VAL' in srcs:
                stored = True
            if mem:
                tags[mem] = set(srcs)
            continue
        if 'VAL' in srcs and 'DT' in srcs and three_op:
            met = True
        if 'VAL' in srcs and x.address > site:
            used = True
        if x.mnemonic in ('vxorps', 'vpxor') and len(src_regs) == 2 and src_regs[0] == src_regs[1]:
            tags[dst] = set()
        else:
            tags[dst] = set(srcs)
    if met:
        return 'dt'
    if stored and not used:
        return 'copy'
    return 'nodt'


rows = []
by_addr = defaultdict(list)
for fs, fe in c.funcs:
    if fe - fs > 600000 or fs in reg_funcs:
        continue
    for x in c.md.disasm(code[fs:fe], fs):
        t = target(x)
        if t in values and not x.op_str.startswith(('dword ptr [rip', 'qword ptr [rip')):
            by_addr[t].append((fs, fe, x.address))
for addr, (name, default) in sorted(values.items(), key=lambda kv: kv[1][0]):
    sites = by_addr.get(addr, [])
    if not sites:
        rows.append((name, addr, default, None, None, 'unread'))
    for fs, fe, site in sites:
        rows.append((name, addr, default, fs, site, classify(fs, fe, site)))

out = sys.argv[1] if len(sys.argv) > 1 else r'C:\Games\ssuvr-work\analysis\tunables_all.tsv'
with open(out, 'w') as f:
    f.write('name\taddress\tdefault\treader\tsite\tclass\n')
    for name, addr, default, fs, site, cls in rows:
        f.write(f'{name}\t{addr:#x}\t{"" if default is None else "%g" % default}\t'
                f'{"" if fs is None else "sub_%x" % fs}\t{"" if site is None else "%#x" % site}\t{cls}\n')
counts = defaultdict(int)
for r in rows:
    counts[r[5]] += 1
print(f'{len(rows)} rows -> {out}; ' + ', '.join(f'{k} {v}' for k, v in sorted(counts.items())), file=sys.stderr)
