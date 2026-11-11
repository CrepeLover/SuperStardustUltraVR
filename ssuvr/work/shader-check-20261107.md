# Shader check, 2026-11-07 (after the point-sprite fix)

## 1. Barycentrics used as data (the point-sprite bug class)
Gameplay shader dump: run `runs\20261107-025136-virtual-dump-play-native` (dump in `pc-vr\user\shader\dumps`).
All 27 pixel shaders seen in gameplay pass logs are dumped; 25 read barycentrics.
`analysis\barycheck.py <dumps>`: only `fs 0x739e13ec` (the bolt point sprites, pipeline ps 073af52e091b)
uses them as data, and that one is fixed. Every other shader only interpolates with them.

## 2. Drop sweep (draws that cover pixels but show nothing)
Emulator switch `SHADPS4_DEBUG_SWEEP=<step s>,<start s>[,right][,alt]` (vk_rasterizer.cpp, DebugSwept).

Paused frame (`runs\20261107-030202-virtual-sweep2-pause`, `sweep2\sweep2-sheet.png`, `analysis\sweep2.py ... pause`,
`sweepfine.py`): 23 pipelines, each dropped in both eyes; a pixel counts when it differs from all
24 nothing-dropped captures.
- 15 add clearly visible pixels (nebula, asteroids, planet, pause sphere, HUD text, stars per eye, bolts, glows).
- 3 add nothing by design: depth-only clear rectangles (ps ceb3555b1a9, RectList, no colour target).
- 2 eye composites (ps 06fcf70e8ad2 left, 073cbeef7ae4 right): copy the side-by-side scene into the two
  960x1080 eye pictures; in a still frame a dropped composite leaves the identical old picture, so the
  sweep cannot see them. They plainly work (the eye pictures show the scene).
- 3 coarse bloom passes (blur 3c2634e42d3 / dc24c92d778 at 120x135 and 60x67, add 784d2991f0b of the 60x67
  level into 120x135): no measurable change. Checked: painting the add pass (SHADPS4_DEBUG_HIGHLIGHT,
  run 20261107-030923) turns the whole eye picture magenta, so that level reaches the screen; its
  shader is out = texture x C[110], and C[110] = 0.5 (run 20261107-031301, SHADPS4_DEBUG_DESCRIBE with
  _FROM). A half-weight, very diffuse glow in a dark paused scene: expected, not a bug.
- Stars: each eye has its own pipeline (vs ...fa left, ...fd right); dropping one clears its eye only.

First gameplay sweep (`runs\20261107-030436-virtual-sweep2-play`, right eye only): inconclusive. The scripted
level ended in the VICTORY screen within seconds, and eye-vs-eye noise was ~6000 px per capture.

## 3. Gameplay sweep (enemies, explosions), fixed
Why the first one failed, and what fixed it:
- The eyes differed by much more than parallax: the default field of view is asymmetric (tan out
  1.207, in 1.181). With `pc-vr\user\vr.json` = `{ "ipd_mm": 0, "fov_tan": [1.2, 1.2, 1.262872, 1.262872] }`
  both eyes match to 0-9 px (> 24 in any channel). So during live play the sweep drops a pipeline in the
  right eye only and compares with the left eye of the same frame: no still frame needed, and
  transient effects (explosions, sparks, bolts) are tested while they happen.
- New script `script-circle.txt`: flies in turning circles and fires; the level lasts ~95 s of play
  (about 50-145 s, then VICTORY) instead of a few seconds.
- New sweep options: `until=<s>` repeats rounds (pipelines that did not draw in their step, or were
  first drawn later, join the next round); `repeat=<n>` drops each pipeline again in later rounds, so
  every pipeline is tested at several moments of the level (with other enemies and explosions on screen).
- Tried and dropped: freezing the game world (`,freeze`, WaitStats::SsuFreezeWorld: update step near 0
  plus the main loop's update jump at 0x426f09 made unconditional). It works for still frames, but
  explosions, sparks and bolts stop drawing while frozen, so they could not be tested that way.

Run `runs\20261107-035401-virtual-sweep6-repeat`: `SHADPS4_DEBUG_SWEEP=0.6,48,right,learn=4,wait=3,repeat=8,until=97`,
`script-circle.txt`, native scene; `analysis\sweep4.py` (`sweep4\sweep4-out.txt`, `sweep4.csv`, one
`strip-<ps>-<vs>.png` per pipeline with every capture, own pixels in magenta). 5 rounds, 160 captures,
noise 8 px max. All 17 gameplay pipelines that draw into the right eye were dropped 3-5 times over the
level, and every one removes pixels of its own:
- Lit meshes `74b8d161fd0` (planet, asteroids, player ship, enemy craft, the chain enemies): 52-81k px.
- Explosions: fire burst `69ffb1a163e` (two vs, up to 36k px), core `2a5d8195dd0` (3.8k), sparks
  `51492b7f0c5` (7.2k), debris `6749f7bd47e`/`...192` (205); bolts `073af52e091b` (point sprites, 952);
  glows `f427c5db813`, `194808fadd1`, `2ef7801633c`, `60cba6bb64c`; shield bubble `cca70e58ecd` (1.4k).
- `18465fc97dd`: the orb glow sprite (also the menu cursor). 2 of 10 captures show it (~950 px, a pickup
  on screen), the rest a few px: it draws every frame but is only lit when an orb is there. Not a bug.
- Earlier run `runs\20261107-034627-virtual-sweep5-liveright` (one drop each, to VICTORY): same picture,
  plus the VICTORY background `60cba6bb64c`/`...24fb` (576k) and `1a0d329b634`/`...31f2` (12.7k).
- Covered by the paused sweep instead (they draw per eye without a right-eye viewport, so right-eye mode
  skips them): bloom passes, eye composites, depth-only clears, left-eye stars.

## Not covered
- `1a0d329b634` with vs `...31f3`: the menu's asteroid-belt background (menu only; the same ps with vs
  `...31f2` was tested on the VICTORY screen). `194808fadd2`: drew twice in a whole run.
- Later planets/waves (other enemy types may bring new pipelines; rerun the sweep with a longer script),
  Invasion Zone and the other modes.
