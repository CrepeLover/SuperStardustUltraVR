"""One-off (2026-11-07): repair two control-character paths in README-SSUVR.md and add the weapon results."""
p = r'C:\Games\SuperStardustUltraVR\README-SSUVR.md'
b = open(p, 'rb').read()
b = b.replace(b'ssuvr-work' + bytes([7]) + b'nalysis', b'ssuvr-work\\analysis')
b = b.replace(b'runs' + bytes([0xc2, 0x82]) + b'61107', b'runs\\20261107')
s = b.decode('utf-8')
old = ("  diverge between rates, so runs drift apart after ~10 s) and per-update random chances. Not yet\r\n"
       "  played in the headset.\r\n")
assert s.count(old) == 1
new = old + (
    "  Weapons at 120 vs 60 (2026-11-07): Ice Splitter shots 204.5 vs 205.0 deg/s, furthest 105.1 vs 106.8\r\n"
    "  deg (one update apart; runs\\20261107-165856-virtual-weapon3-60-d, -170021-...-w120-d; l1 selects it\r\n"
    "  from the Rock Crusher, r1 the Gold Melter). The Gold Melter beam was ~30% short: it adds a segment\r\n"
    "  60 times a second at any rate (bullets kept a fixed time), but its update (sub_26c7a0) puts each\r\n"
    "  segment one update's travel beyond its newer neighbour, so at 120 the gaps went step, step, two\r\n"
    "  steps (60.4 units of beam vs 83.8). SsuPatchBeamSpacing moves that placement (0x26c8de..0x26c91a)\r\n"
    "  to a cave at eboot+0x6e7d00 that uses the 60 Hz gap (x rate/59.94) and catches up k = 59.94/rate\r\n"
    "  of the way per update, so bends still run down the beam at the 60 Hz pace (slots eboot+0xcfdf80,\r\n"
    "  both 1 at 60 Hz). Verified with the probe SHADPS4_SSU_BEAM=1 + SHADPS4_SSU_COUNT=26f920 (longest\r\n"
    "  chain summary in the log, geometry in user\\log\\ssu-beam.csv; analysis\\beam_bend.py): still aim,\r\n"
    "  steady beam 79.4 / 86.6 units in two 120 runs vs 83.8 at 60 (segment count varies 30-34 per run),\r\n"
    "  gap 2.75 vs 2.74; aim turning 30 deg/s: bend 18.8 vs 19.0 deg, reach 32.5 vs 31.4 deg\r\n"
    "  (runs\\20261107-173203-virtual-beamsweep-60, -173326-...-w120). SSU_FPS=60 with the patch: 83.7.\r\n")
s = s.replace(old, new)
open(p, 'wb').write(s.encode('utf-8'))
print('ok; CRLF', s.count('\r\n'), 'LF', s.count('\n'))
