"""Per run, from SHADPS4_LOG_PASSES windows (every 5 s): how many frames a second, and per frame how
many bolts (point-sprite pipeline: indices per draw = points), sparks, explosion particles and lit
meshes are drawn, averaged over the play windows (bolts drawn at all).

usage: passrate.py <label>=<run dir> ...   (left/right eye draws are both counted: per draw is per eye)"""
import re
import sys

PIPES = {
    'bolts': 'ps 0000073af52e091b',
    'sparks': 'ps 0000051492b7f0c5',
    'fire': 'ps 0000069ffb1a163e vs 000005e9c171a84d',
    'fire2': 'ps 0000069ffb1a163e vs 000006ce6dd48a73',
    'core': 'ps 000002a5d8195dd0',
    'meshes': 'ps 0000074b8d161fd0',
    'stars_r': 'ps 00000d86de26bf0b vs 00000e55c70324fd',
}
LINE = re.compile(r'Note:\s+(\d+) x\s+(\d+) indices: draw')

for arg in sys.argv[1:]:
    label, run = arg.split('=', 1)
    windows, cur = [], None
    for line in open(run + r'\shad_log.txt', encoding='utf-8', errors='replace'):
        if 'Passes in the last' in line:
            cur = {}
            windows.append(cur)
            continue
        if cur is None:
            continue
        m = LINE.search(line)
        if not m:
            continue
        for name, key in PIPES.items():
            if key in line:
                d, n = int(m.group(1)), int(m.group(2))
                a = cur.setdefault(name, [0, 0])
                a[0] += d
                a[1] += n
    play = [w for w in windows if 'bolts' in w and 'stars_r' in w]
    if not play:
        print(label, 'no play windows')
        continue
    frames = sum(w['stars_r'][0] for w in play)  # one right-eye star draw per frame
    secs = 5.0 * len(play)
    out = [f'{label:>6}: {len(play)} play windows, {frames / secs:6.1f} frames/s']
    for name in ('bolts', 'sparks', 'fire', 'fire2', 'core', 'meshes'):
        draws = sum(w.get(name, [0, 0])[0] for w in play)
        idx = sum(w.get(name, [0, 0])[1] for w in play)
        per_frame = idx / max(frames, 1) / 2  # both eyes draw: per eye
        per_sec = idx / secs / 2
        out.append(f'{name} {per_frame:8.1f}/frame {per_sec:10.0f}/s')
    print(' | '.join(out))
