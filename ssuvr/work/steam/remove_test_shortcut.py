"""Remove the redundant "Super Stardust Ultra VR 120 fps (test)" Steam shortcut (2026-11-07, user request).
The main shortcut already runs world-rate 120 from the saved settings. Steam must be shut down first (it rewrites
shortcuts.vdf on exit). Backs up shortcuts.vdf beside this script and renumbers the keys 0..n-1 as Steam writes them.
Usage: python -I remove_test_shortcut.py [--dry-run]"""
import shutil
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import vdf_shortcuts as vdf  # noqa: E402

VDF = Path(r'C:\Program Files (x86)\Steam\userdata\<steam-user-id>\config\shortcuts.vdf')
NAME = 'Super Stardust Ultra VR 120 fps (test)'
dry = '--dry-run' in sys.argv

root = vdf.load(VDF)
old = root['shortcuts']
keep = [e for e in old.values() if e.get('AppName') != NAME]
gone = len(old) - len(keep)
if gone != 1:
    sys.exit(f'expected one "{NAME}" entry, found {gone}; nothing written')
root['shortcuts'] = {str(i): e for i, e in enumerate(keep)}
if dry:
    print(f'dry run: {len(old)} -> {len(keep)} shortcuts; main entry kept:',
          any(e.get('AppName') == 'Super Stardust Ultra VR (PC VR port)' for e in keep))
    sys.exit(0)
shutil.copy2(VDF, Path(__file__).parent / f'shortcuts.vdf.bak-{time.strftime("%Y%m%d-%H%M%S")}')
vdf.save(VDF, root)
check = vdf.load(VDF)['shortcuts']
names = [e.get('AppName') for e in check.values()]
assert NAME not in names and len(check) == len(keep)
print(f'removed "{NAME}"; {len(old)} -> {len(check)} shortcuts; main entry kept:',
      'Super Stardust Ultra VR (PC VR port)' in names)
