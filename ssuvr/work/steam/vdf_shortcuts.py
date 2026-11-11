"""Minimal binary-VDF (shortcuts.vdf) reader/writer. usage: vdf_shortcuts.py list <vdf> | add <vdf> <json-entry>"""
import sys, json, struct

def read_str(b, i):
    j = b.index(b'\x00', i); return b[i:j].decode('utf-8', 'replace'), j + 1

def parse_map(b, i):
    d = {}
    while True:
        t = b[i]; i += 1
        if t == 8: return d, i
        k, i = read_str(b, i)
        if t == 0: v, i = parse_map(b, i)
        elif t == 1: v, i = read_str(b, i)
        elif t == 2: v = struct.unpack_from('<i', b, i)[0]; i += 4
        else: raise ValueError('type %d at %d' % (t, i))
        d[k] = v
    
def dump_map(d):
    out = bytearray()
    for k, v in d.items():
        kb = k.encode('utf-8') + b'\x00'
        if isinstance(v, dict): out += b'\x00' + kb + dump_map(v)
        elif isinstance(v, str): out += b'\x01' + kb + v.encode('utf-8') + b'\x00'
        elif isinstance(v, bool): out += b'\x02' + kb + struct.pack('<i', int(v))
        else: out += b'\x02' + kb + struct.pack('<i', v)
    return bytes(out) + b'\x08'

def load(p):
    b = open(p, 'rb').read(); root, i = parse_map(b, 0)
    assert i == len(b), 'trailing bytes'
    return root

def save(p, root):
    open(p, 'wb').write(dump_map(root))

if __name__ == '__main__':
    cmd, p = sys.argv[1], sys.argv[2]
    root = load(p); sc = root['shortcuts']
    if cmd == 'list':
        for k, e in sc.items():
            print(k, json.dumps(e, ensure_ascii=False)[:400])
    elif cmd == 'add':
        entry = json.load(open(sys.argv[3], encoding="utf-8")) if sys.argv[3].endswith(".json") else json.loads(sys.argv[3])
        names = [e.get('AppName') for e in sc.values()]
        if entry['AppName'] in names: print('already present:', entry['AppName']); sys.exit(2)
        key = str(max([int(k) for k in sc] + [-1]) + 1)
        sc[key] = entry; save(p, root); print('added as', key)
        # round trip check
        assert load(p)['shortcuts'][key]['AppName'] == entry['AppName']
