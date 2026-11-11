"""Check the video-heap size immediates in the SSUVR eboot (text: file offset = vaddr + 0x4000)."""
eboot = r'C:\Games\SuperStardustUltraVR\games\CUSA05951\eboot.bin'
d = open(eboot, 'rb').read()
sites = {
    0x555bfa: ('mov edx, imm32 (AllocateDirectMemory len)', 1),
    0x555c2b: ('mov esi, imm32 (MapDirectMemory len)', 1),
    0x555cb1: ('mov edx, imm32 (printf Shared Video size)', 1),
    0x555ecd: ('mov dword [rip+x], imm32 (stored heap size)', 6),
    0x555ce6: ('mov ecx, imm32 (mask helper, leave alone)', 1),
}
for va, (what, immoff) in sites.items():
    off = va + 0x4000
    ins = d[off:off + immoff + 4]
    imm = int.from_bytes(d[off + immoff:off + immoff + 4], 'little')
    print(f'{va:#08x} {ins.hex(" ")}  imm {imm:#010x}  imm at {va + immoff:#08x} -> patch addr {va + immoff + 0x400000:08X}  {what}')
