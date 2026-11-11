"""Verify the 120 fps patch: the instruction at 0x25ca3f loads 59.94 for the frame timer's set_rate;
repoint its rip displacement to the game's own 120.0f constant at 0x5b66dc."""
import sys, struct
sys.path.insert(0, r'C:\Users\<user>\AppData\Local\Temp\claude\C--Users-<user>\ac2a6782-da34-4ad3-a819-19f4f42a87ac\scratchpad\refute-c3')
from common import *
c = ctx()
INS, TARGET = 0x25ca3f, 0x5b66dc
orig = c.elf.read(INS, 8)
ins = list(c.md.disasm(orig, INS))[0]
assert orig[:4] == bytes.fromhex('c5fa1005'), orig.hex()
old_disp = struct.unpack('<i', orig[4:8])[0]
assert INS + 8 + old_disp == 0x5b6488 and abs(struct.unpack('<f', c.elf.read(0x5b6488, 4))[0] - 59.94) < 1e-4
assert struct.unpack('<f', c.elf.read(TARGET, 4))[0] == 120.0
new_disp = TARGET - (INS + 8)
patched = orig[:4] + struct.pack('<i', new_disp)
pins = list(c.md.disasm(patched, INS))[0]
print('original: %s  %s %s  -> %#x = %g' % (orig.hex(' '), ins.mnemonic, ins.op_str, INS + 8 + old_disp, 59.94))
print('patched : %s  %s %s  -> %#x = %g' % (patched.hex(' '), pins.mnemonic, pins.op_str, INS + 8 + new_disp, struct.unpack('<f', c.elf.read(TARGET, 4))[0]))
print('patch line: Address=%08X Value=%s (original %s)' % (INS + 4 + 0x400000, struct.pack('<i', new_disp).hex().upper(), orig[4:8].hex().upper()))
# file check: the installed eboot has the same bytes at the file offset (text: file offset = vaddr + 0x4000)
raw = open(r'C:\Games\SuperStardustUltraVR\games\CUSA05951\eboot.bin', 'rb').read()
print('installed eboot bytes at %#x: %s' % (INS + 0x4000, raw[INS + 0x4000: INS + 0x4008].hex(' ')))
print('installed eboot 120.0 at %#x: %g' % (TARGET, struct.unpack('<f', raw[TARGET + 0x4000: TARGET + 0x4004])[0]))
