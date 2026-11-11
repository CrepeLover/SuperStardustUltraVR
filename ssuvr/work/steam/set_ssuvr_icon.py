"""Set the icon of the Super Stardust Ultra VR Steam shortcuts to steam\\art\\ssuvr.ico (2026-11-07).
Steam must be shut down first (it rewrites shortcuts.vdf on exit). Backs up shortcuts.vdf beside this script.
Usage: python -I set_ssuvr_icon.py"""
import shutil
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import vdf_shortcuts as vdf  # noqa: E402

VDF = Path(r'C:\Program Files (x86)\Steam\userdata\<steam-user-id>\config\shortcuts.vdf')
ICON = r'C:\Games\SuperStardustUltraVR\steam\art\ssuvr.ico'
NAMES = ('Super Stardust Ultra VR (PC VR port)', 'Super Stardust Ultra VR 120 fps (test)')

shutil.copy2(VDF, Path(__file__).parent / f'shortcuts.vdf.bak-{time.strftime("%Y%m%d-%H%M%S")}')
root = vdf.load(VDF)
done = []
for key, entry in root['shortcuts'].items():
    if entry.get('AppName') in NAMES:
        entry['icon'] = ICON
        done.append(f'{key} {entry["AppName"]}')
vdf.save(VDF, root)
check = vdf.load(VDF)['shortcuts']
print('icon set on:', done, '| shortcuts:', len(check))
