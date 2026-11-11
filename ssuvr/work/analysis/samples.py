"""Summarise main_samples.csv from SHADPS4_SAMPLE_MAIN: where the title's main thread spends its time.
usage: samples.py <main_samples.csv> [from_ms] [to_ms]"""
import sys, csv, collections, struct
sys.path.insert(0, r'C:\Users\<user>\AppData\Local\Temp\claude\C--Users-<user>\ac2a6782-da34-4ad3-a819-19f4f42a87ac\scratchpad\refute-c3')
from common import *

c = ctx()
path = sys.argv[1]
lo = float(sys.argv[2]) if len(sys.argv) > 2 else 0.0
hi = float(sys.argv[3]) if len(sys.argv) > 3 else 1e12


def parse(text):
    if text in ('-', ''):
        return None, None
    if '+' not in text:
        return '?', int(text, 16)
    mod, off = text.rsplit('+', 1)
    return mod, int(off, 16)


def fname(addr):
    f = c.func_of(addr)
    return 'sub_%x' % f[0] if f else '?%x' % addr


def import_at(ret):
    """The import (or function) called by the call instruction that returns to `ret`."""
    try:
        b = c.elf.read(ret - 6, 6)
    except Exception:
        return '?'
    if b[1] == 0xE8:
        target = (ret + struct.unpack('<i', b[2:6])[0]) & 0xffffffffffffffff
        if target in c.stubs:
            return c.stubs[target]
        return 'sub_%x' % target
    return 'indirect'


rows = []
with open(path, newline='') as fh:
    for r in csv.DictReader(fh):
        ms = float(r['ms'])
        if lo <= ms <= hi:
            rows.append(r)
n = len(rows)
print('%d samples between %.0f and %.0f ms' % (n, lo, min(hi, float(rows[-1]['ms']) if rows else 0)))

where_class = collections.Counter()
guest_funcs = collections.Counter()
host_by_site = collections.Counter()
host_mod = collections.Counter()
for r in rows:
    mod, off = parse(r['where'])
    frames = [parse(r['frame%d' % i]) for i in range(1, 5)]
    if mod and 'eboot' in mod.lower():
        where_class['title code (eboot)'] += 1
        guest_funcs[fname(off)] += 1
    else:
        cls = mod.lower() if mod else '?'
        if cls.endswith('.prx') or cls.endswith('.sprx'):
            where_class['title library ' + mod] += 1
        else:
            where_class[mod] += 1
            host_mod[mod] += 1
        # attribute to the title's call site: the first eboot frame
        site = next(((m, o) for m, o in frames if m and 'eboot' in m.lower()), None)
        if site:
            host_by_site['%s at %#x in %s -> %s' % (mod, site[1], fname(site[1]), import_at(site[1]))] += 1
        else:
            host_by_site['%s (no title frame)' % mod] += 1

print('\nwhere the main thread is:')
for k, v in where_class.most_common(15):
    print('  %5.1f%%  %s' % (100.0 * v / n, k))
print('\ntitle functions (own code):')
for k, v in guest_funcs.most_common(25):
    print('  %5.1f%%  %s' % (100.0 * v / n, k))
print('\noutside the title, by the title call that led there:')
for k, v in host_by_site.most_common(30):
    print('  %5.1f%%  %s' % (100.0 * v / n, k))
