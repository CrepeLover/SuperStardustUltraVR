"""Print a PS4 param.sfo's entries (integers in hex) and decode ATTRIBUTE / ATTRIBUTE2 bits that matter here."""
import struct, sys

data = open(sys.argv[1], 'rb').read()
magic, version, key_table, data_table, count = struct.unpack_from('<4sIIII', data, 0)
assert magic == b'\0PSF', magic
values = {}
for i in range(count):
    key_off, fmt, length, max_len, data_off = struct.unpack_from('<HHIII', data, 20 + i * 16)
    end = data.index(b'\0', key_table + key_off)
    key = data[key_table + key_off:end].decode()
    raw = data[data_table + data_off:data_table + data_off + length]
    if fmt == 0x0404:
        val = struct.unpack('<I', raw)[0]
        values[key] = val
        print('%-24s 0x%08x (%d)' % (key, val, val))
    else:
        text = raw.rstrip(b'\0').decode('utf-8', 'replace')
        values[key] = text
        print('%-24s %r' % (key, text[:80]))
a = values.get('ATTRIBUTE', 0)
print('ATTRIBUTE bits: support_ps_vr=%d require_ps_vr=%d support_neo_mode(bit23)=%d support_hdr=%d six_cpu=%d seven_cpu=%d' % (
    (a >> 14) & 1, (a >> 26) & 1, (a >> 23) & 1, (a >> 29) & 1, (a >> 15) & 1, (a >> 16) & 1))
a2 = values.get('ATTRIBUTE2', 0)
print('ATTRIBUTE2 = 0x%08x (bit2 = PS4 Pro enhanced / 4K: %d)' % (a2, (a2 >> 2) & 1))
