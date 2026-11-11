# Super Stardust Ultra VR on PC VR (prototype)

Runs the PS4 PSVR title Super Stardust Ultra VR (CUSA05951, EU, patch 1.03) in the shadPS4 +
OpenXR fork used for the Farpoint and Astro Bot ports, on a PC VR headset or flat on the monitor.
The emulator, `pc-vr\shadps4.exe`, is built from `C:\Games\Farpoint-src` (branch `ssuvr`, the
evertec82/Farpoint v0.9.6 tree, whose VR bridge comes from bigmak94's AstroQuest) and copied
into `pc-vr\`. It is not part of this folder yet: build it first, then copy it there.

## Install layout

```
C:\Games\SuperStardustUltraVR\
  Play Super Stardust Ultra VR.bat          settings window, then the game
  Play Super Stardust Ultra VR (Steam).bat  saved settings, no window (add as a non-Steam game, start from SteamVR)
  README-SSUVR.md                           this file
  games\CUSA05951\                          the game: base 1.00 merged with patch 1.03 from your own dump
    eboot.bin                               SHA256 4A6349CB...CA5D (1.03); 9BB96380...13DA is the base 1.00 executable
    sce_sys\param.sfo                       TITLE_ID CUSA05951, APP_VER 01.03 (the launcher checks both files)
    HMD.config, Plan18.HMDconfig, ...       the game's own data (dev-kit optics configs are loaded by the game itself)
  pc-vr\
    launch-ssuvr.ps1                        the launcher (PowerShell 5.1, ASCII)
    config-default.json                     emulator profile copied to user\config.json on the first run
    shadps4.exe                             the emulator, built from C:\Games\Farpoint-src branch ssuvr (copy it here)
    ssuvr-settings.json                     saved choices from the settings window (created on first start)
    user\config.json                        the emulator profile actually used (portable user directory)
    user\vr.json                            optional VR bridge file config (headset on/off/auto, ipd_mm, fov_tan, ...)
    user\log\shad_log.txt                   the emulator log of the last run
    user\log\previous-<stamp>.txt           the log of the run before
    user\log\session-<stamp>.json           what was started, with the full environment table
    user\log\exit-<stamp>.json              exit code (and whether the launcher stopped it)
    user\log\console-<stamp>.stdout.txt     emulator console output (.stderr.txt alongside)
```

No pkg extraction and no pak profiles: the game folder comes from the user's own dump. The render
resolution is set by the launcher on every start: a memory patch for the eye pictures, the game's own
`Plan18.HMDconfig` for the 3D scene, and at 400% a bigger GPU heap plus 2 GB extra emulator memory
(see `pc-vr\user\patches\ssuvr-res\README-resolution.md`).

## How to run

- Headset: start the OpenXR runtime (SteamVR / Virtual Desktop / the vendor runtime), connect the
  headset, then run `Play Super Stardust Ultra VR.bat` and press Start (headset mode `on`).
- From Steam / SteamVR: add `Play Super Stardust Ultra VR (Steam).bat` as a non-Steam game; it
  starts with the choices last saved in the settings window, without showing it.
- Flat on the monitor: choose `Off - flat on the monitor` in the window (or `-Headset off`). The
  game then reports "No HMD detected!" and plays as a 2D title at 1280x720 with a gamepad.
- Command line (from `pc-vr\`): `powershell -NoProfile -STA -ExecutionPolicy Bypass -File launch-ssuvr.ps1 [options]`

| Option | Values | Meaning |
| --- | --- | --- |
| `-GamePath <file>` | path to eboot.bin | default `games\CUSA05951\eboot.bin`, or the saved one |
| `-Headset` | `on` (default), `off`, `auto` | `on`: SHADPS4_OPENXR=1 SHADPS4_VR=1; `off`: both 0 (flat); `auto`: SHADPS4_OPENXR=1, SHADPS4_VR unset (the bridge decides) |
| `-MirrorFps` | 0, 30 (default), 60 | desktop mirror cap (0 = uncapped); does not cap headset frames |
| `-FovPercent` | 60-120, default 100 | percent of the headset's own field of view the game is told to render (100 = the whole view) |
| `-RenderPercent` | 100, 150, 200 (default), 250, 300, 400 (3840x4320, 4K an eye) | render size in percent of the PSVR's 960x1080 an eye (200 = 1920x2160), for both the 3D scene (the game's `Plan18.HMDconfig`, rewritten every start from `pc-vr\Plan18.HMDconfig.original`) and the eye pictures (memory patch in `pc-vr\user\patches\ssuvr-res\`); 400% also turns on the GPU heap patch (`ssuvr-heap`) and `extra_dmem_in_mbytes = 2048` |
| `-Fps` | 60 (default), 120, 144, headset | frames a second. `headset` takes the headset's refresh rate at start (a Steam Frame: 120, or 144 Hz when SteamVR runs it at 144); 120/144 force a rate. How the game world follows is `-RateMode`. With `-NoGui` it applies to that run only; the settings window ("Game frame rate") saves it |
| `-RateMode` | world (default), draw | above 60 fps: `world` runs the game world at the frame rate with its per-frame values rescaled so play matches 60 (SHADPS4_SSU_FPS; smooth world and camera motion). `draw` keeps the world at 59.94 updates a second and only gives the frames between a fresh head pose (SHADPS4_SSU_DRAW_FPS; play exactly as at 60, but the world and camera move every other frame at 120 Hz). Settings window: the "run the game world at that rate too" box. See the notes below |
| `-NoOpenXr` | switch | SHADPS4_OPENXR=0 while SHADPS4_VR follows `-Headset`: the virtual headset shown on the monitor (desktop stereo view, demo motion), no runtime needed |
| `-Seconds <n>` | seconds | smoke tests: stop the emulator after n seconds and report "stopped after n s" instead of failing |
| `-NoGui` | switch | no settings window; saved settings plus the command line |
| `-ValidateOnly` | switch | check the game files and print the settings and environment table without starting anything |
| `-Env` | hashtable or `NAME=value` strings | extra environment applied last (an empty value removes the variable) |

Smoke tests without a headset: `-NoGui -Headset off -Seconds 120` (flat) and
`-NoGui -Headset on -NoOpenXr -Seconds 120` (virtual headset on the monitor). Nothing in this folder
touches SteamVR; the OpenXR runtime is only contacted when SHADPS4_OPENXR=1.

The launcher refuses to start while another `shadps4.exe` runs (it prints that process's command
line), checks the eboot.bin hash (1.03 supported, 1.00 allowed with a warning) and the
`sce_sys\param.sfo` TITLE_ID, and on every run forces `Input.use_special_pad=false`,
`Network.connected_to_network=false` and `Network.shad_net_enabled=false` in `user\config.json`
while keeping the other settings.

## Environment the launcher sets

| Variable | Value | Why |
| --- | --- | --- |
| `SHADPS4_OPENXR` | 1 / 0 | look for an OpenXR runtime (0 with `-Headset off` or `-NoOpenXr`) |
| `SHADPS4_VR` | 1 / 0 / unset | the virtual PSVR: connected / absent / `auto` (unset, the bridge decides) |
| `SHADPS4_VR_FOV` | `-FovPercent` | rendered FOV in percent of the headset's own view |
| `SHADPS4_VR_FOV_OF` | headset | the percent is of what the headset shows, not of a PSVR's 100 degrees |
| `SHADPS4_VR_WINDOW_FPS` | `-MirrorFps` | desktop mirror cap |
| `SHADPS4_VR_PACE` | 1 | one host refresh per two guest vblanks |
| `SHADPS4_XR_WAIT` | 0 | do not wait for a headset at start |
| `SHADPS4_XR_PAUSE` | 0 | do not pause when the headset is taken off |
| `SHADPS4_XR_GAMEPAD_LAYOUT` | 1 | VR controllers act as one DualShock |
| `SHADPS4_XR_PAD_HAND` | right | the virtual pad sits in the right hand |
| `SHADPS4_GPU_WAIT_TIMING` | 1 | as Farpoint |
| `SHADPS4_SUBMIT_BATCH_SIZE` | 32 unless already set | as Farpoint |
| `SHADPS4_TITLE_RESOLUTION` | title | Astro-specific resolution patches must not leak |
| removed | `SHADPS4_XR_SWAP_CIRCLE_R3`, `SHADPS4_XR_SWAP_GRIPS`, `SHADPS4_XR_TOGGLE_L3`, `SHADPS4_XR_PAD_PITCH`, `SHADPS4_XR_PAD_OFFSET`, `SHADPS4_FARPOINT_OFFLINE_SCORE`, `SHADPS4_FARPOINT_EYE_WIDTH`, `SHADPS4_INPUT_SCRIPT`, `SHADPS4_LIVE_INPUT`, `SHADPS4_XR_TEST_PRESS`, `SHADPS4_XR_TEST_TURN`, `SHADPS4_TITLE_EYE_WIDTH`, `SHADPS4_VR_FPS_CAP` | Farpoint-only and test knobs |

Other knobs of this emulator build that `-Env` can pass: `SHADPS4_VR_DEMO=1` (demo head motion
on the virtual headset), `SHADPS4_VR_DESKTOP_VIEW=stereo|spectator|combined` and
`SHADPS4_VR_DESKTOP_CROP=1` (what the window shows), `SHADPS4_VR_REFRESH_RATE=60..120`,
`SHADPS4_VR_FOV_SYMMETRIC=1`, `SHADPS4_HMD_PANEL=<w>x<h>` (panel size the HMD library reports; this game ignores it), `SHADPS4_XR_HANDS=0`, `SHADPS4_XR_HEAD=0`,
`SHADPS4_XR_CONTROLLERS=0`, `SHADPS4_XR_PREDICT_MS`, `SHADPS4_VR_MLAA=0|0.5`, `SHADPS4_FORCE_ANISO=0`,
`SHADPS4_VR_EYE_DECODE=auto|0|1` (sRGB decoding of the eye pictures; auto = by texture format).
Diagnostics: `SHADPS4_LOG_PASSES=<seconds>` (every few seconds, each kind of draw/dispatch with its
colour/depth target size and samples, viewport, scissor and the render targets it reads: shows the
resolution the game really draws at), `SHADPS4_LOG_IMAGES=1` (one line per image the game creates).
The same headset mode, IPD, refresh rate and FOV can live in `pc-vr\user\vr.json` instead;
environment variables win over the file.

## Known unknowns

- The headset path has not been run at all yet; only the flat boot (no headset, AstroQuest v0.8.5
  build: 2 minutes, 1280x720, 0 criticals, "hmd status = 2" then "No HMD detected!") is known.
- `sceVideoOutRegisterStereoBuffers` (libSceVideoOut import lCTCOogRbk0) is not implemented in
  the emulator: it falls to a stub returning 0. The game calls it once at startup.
- The game uses the plain `sceHmdReprojectionStart` path; `hmd_reprojection.cpp` gates packed
  side-by-side stereo on the Farpoint serial only, so the stereo presentation for CUSA05951 is open.
- libSceVrTracker HLE checks struct sizes (128-byte init struct; Farpoint's SDK 4.x needed 144);
  the SDK 3.508 sizes this game passes are unknown until the headset log shows them.
- Whether the OpenXR controller bindings (`SHADPS4_XR_GAMEPAD_LAYOUT=1`) suit a DualShock-only
  twin-stick shooter, and whether the game accepts the virtual headset's optics in place of its
  `HMD.config` / `Plan18.HMDconfig` dev-kit configs, is untested.
- `pc-vr\shadps4.exe` does not exist until the ssuvr branch is built; `-ValidateOnly` works without it.

## Status (2026-11-01)

Verified on this PC (shadps4.exe built from C:\Games\Farpoint-src branch `ssuvr`):

- Virtual headset on the monitor (`-Headset on -NoOpenXr`): the game detects the headset, enters VR
  mode, renders 960x1080 per eye at 60 fps, head tracking works (the view follows the tracker pose),
  the gamepad works (menus navigated, VR Invasion Zone reached up to its tips screen).
- Real OpenXR session through SteamVR (headset in standby, nobody wearing it): the session reached
  `visible`, the Frame's FOV was read, 60 frames/s were delivered and shown, head tracked 100%, no
  XR errors. Not yet tried by a person in the headset: controller feel, audio in the headset, comfort.
- Skip the intro (default on): the memory patch in `pc-vr\user\patches\ssuvr\CUSA05951.xml`
  (loaded automatically, "Applied patch" six times in the log) boots the game straight to the title
  screen: no autosave notice, logos, video, legal text or calibrate prompt. Black until about 12 s,
  title at about 13 s, with no button presses. The calibrate step is done for you, so reset the view
  once you are seated: press both sticks in. Autosave still works (the save file is written at ~12 s).
  Details and the byte table: `pc-vr\user\patches\ssuvr\README-skip-logos.md`. To see the logos
  again set `isEnabled="false"` in the XML and put the presses back into `pc-vr\skip-intro.txt`.
- Emulator changes made for this title (all generic, in the `ssuvr` branch): SDK 3.5 tracker
  parameter sizes accepted, `sceVideoOutRegisterStereoBuffers` implemented, packed-stereo detection
  by texture address, trophy context registered without a trophy pack, `SHADPS4_INPUT_SCRIPT` wired
  up, `SHADPS4_AVPLAYER_SKIP` added.
- Render resolution (2026-11-03): the game draws its whole 3D scene into one side-by-side buffer
  sized by its own `Plan18.HMDconfig` (originally 1920x1080 = 960x1080 an eye) and only then stretches
  it into the eye pictures. Until 2026-11-03 the launcher enlarged only the eye pictures, so a 960x1080
  scene was blown up to 3840x4320. Now the "Render resolution" setting sets both; the saved choice
  400% draws the scene at 3840x4320 an eye (7680x4320 buffer) with the GPU heap patch and 2 GB extra
  emulator memory, verified through the launcher on the monitor at 57-60 fps with sharp eye captures.
  Details: `pc-vr\user\patches\ssuvr-res\README-resolution.md`. The headset is also told its own field
  of view (`SHADPS4_VR_FOV_OF=headset`), so the picture fills the Frame's 110 degrees instead of a
  PSVR's 100 with a border; the game renders with the field of view it is told (a 60% run is visibly
  zoomed in).
- World rate with rescaling (2026-11-07, `-RateMode world`, the launcher's default above 60): SHADPS4_SSU_FPS
  sets the game's frame timer to the rate (below), and SsuFollowGameRate rescales everything the game
  applies once per update without the time step, for k = 59.94/rate: a lerp fraction f -> 1-(1-f)^k, a
  kept fraction m -> m^k, a frame count n -> n/k, a per-frame increment a -> a*k. Inventory
  (ssuvr-work\analysis): tunables_all.py lists every registered debug value with its reader;
  tunable_patterns.py classifies the arithmetic at each read; tunable_table.py --cpp makes the table of
  148 values (boost speed up/down lerps, camera move/setting lerps, rock velocity/rotation slowdowns,
  enemy turn/approach lerps, token fly slowdowns, rumble fades, HUD lerps, ...). Bullet Physics
  dampings (COMMAND_GAME setDamping) are per second and left alone. Values written into the code
  instead (hardcoded_lerps.py, hardcoded_damping.py, read by hand) are redirected to slots at
  eboot+0xcfdf10 (SsuPatchRateConstants): centipede/token heading lerps, a follow lerp, a versus
  marker, a menu slide, and the ship's velocity damping (sub_31b6b0: *0.95 steering, *0.9 coasting -
  the boost's top speed is where it meets the push, ~16% short without this). The particle step
  follows 1/rate. No hard-coded frame counters were found (frame_counters scan). SHADPS4_SSU_RESCALE=0
  turns the table off for comparisons. Verified at 120 against 60 (virtual headset, scripted input):
  shot reach 57.98 vs 60.07 deg (draw-test.ps1, runs\20261107-160254-virtual-world120-final); ship
  speed 64 units/s both, 4 s move 156 vs 160 units; boost peak 0.5 s windows 159/180 vs 174/163 units/s
  (boost-test.ps1, runs\20261107-155046-virtual-boost2-60, 20261107-155930-virtual-boost3-w120; before the
  ship damping fix 113/133). Not yet compared: individual enemy behaviour (rock and enemy spawns
  diverge between rates, so runs drift apart after ~10 s) and per-update random chances. Not yet
  played in the headset.
  Weapons at 120 vs 60 (2026-11-07): Ice Splitter shots 204.5 vs 205.0 deg/s, furthest 105.1 vs 106.8
  deg (one update apart; runs\20261107-165856-virtual-weapon3-60-d, -170021-...-w120-d; l1 selects it
  from the Rock Crusher, r1 the Gold Melter). The Gold Melter beam was ~30% short: it adds a segment
  60 times a second at any rate (bullets kept a fixed time), but its update (sub_26c7a0) puts each
  segment one update's travel beyond its newer neighbour, so at 120 the gaps went step, step, two
  steps (60.4 units of beam vs 83.8). SsuPatchBeamSpacing moves that placement (0x26c8de..0x26c91a)
  to a cave at eboot+0x6e7d00 that uses the 60 Hz gap (x rate/59.94) and catches up k = 59.94/rate
  of the way per update, so bends still run down the beam at the 60 Hz pace (slots eboot+0xcfdf80,
  both 1 at 60 Hz). Verified with the probe SHADPS4_SSU_BEAM=1 + SHADPS4_SSU_COUNT=26f920 (longest
  chain summary in the log, geometry in user\log\ssu-beam.csv; analysis\beam_bend.py): still aim,
  steady beam 79.4 / 86.6 units in two 120 runs vs 83.8 at 60 (segment count varies 30-34 per run),
  gap 2.75 vs 2.74; aim turning 30 deg/s: bend 18.8 vs 19.0 deg, reach 32.5 vs 31.4 deg
  (runs\20261107-173203-virtual-beamsweep-60, -173326-...-w120). SSU_FPS=60 with the patch: 83.7.
  Enemy shots (2026-11-08): they share the bullet pool with the player's (sub_26f920; type at +8:
  6 Lurker plasma, 3-5 boss/cannon plasma, 7/8 boss 2 ray, 23-26 Invasion Zone carrier/tank/aegis),
  move by speed x dt and die when their particle effect ends, like the player's shots (fixed by the
  particle step). Probe SHADPS4_SSU_BULLETS=1 + SSU_COUNT=26f920 (user\log\ssu-bullets.csv,
  analysis\bullet_lives.py, ssuvr-work\bullet-test.ps1); test knob SHADPS4_SSU_SET=9be5bc=1000 keeps the
  respawn shield on so a scripted ship survives. No enemy fired in planet 1 Arcade or 3 min of Endless
  (Lurkers: planet 3, cannons: planets 4-5 and later Endless, per the attack-wave XML in stream.dat);
  the Rock Crusher matched again (life 0.700 vs 0.717 s, 81.8 deg/s). analysis\tunable_increments.py
  found 7 more per-update values, now in the rescale table (155): Invasion Zone carrier bomb fall-off
  (x0.99 / -0.005 an update), swarmer flocking weights, Invasion Zone gatling barrel spin-up. Known,
  cosmetic: sub_4f4e80 (a menu screen, state 0xb) adds a fixed 1/60 s a frame to a 4 s cycle.
- Draw rate (2026-11-07, `-RateMode draw`): the emulator patches the
  main loop (wait_stats.cpp SsuPatchFrameLoop, SHADPS4_SSU_DRAW_FPS) so it ticks at the draw rate but
  runs the world update (Game_Logic_Impl::update and the rest of the update block, always the game's
  own 1/59.94 s step) only on U of every N ticks (120 Hz: 1 in 2; 144 Hz: 5 in 12). Frames without an
  update run the update's head-pose block alone, so every frame has a fresh pose, then the per-frame
  calls and the draw. The per-frame part ages the particle systems (Particle_System_Impl, from
  sub_422d50) by a fixed 1/59.94 s a frame (rodata 0x5b90f4); a Rock Crusher shot lives as long as its
  effect's particles, so at 120 Hz shots died at half range (30 deg instead of 60). SsuPatchParticleStep
  redirects those two loads (0x422e1a, 0x422e80) to a slot set to the step times U/N. Verified
  (ssuvr-work\draw-test.ps1, runs\20261107-151840-virtual-draw120-pstep): shot reach 58.7 deg at 120 Hz
  against 60.07 at 60 (the trace samples every second update, 2.7 deg), 119.9 headset frames a second,
  no frame drawn from a repeated pose. The camera follow (sub_4df4d0) and the ship run in the world
  update. Found with new emulator knobs: SHADPS4_SSU_COUNT=<hex,..> (calls a second, caller, last
  xmm0) and SHADPS4_SSU_WATCH=<function>,<pointer offset|ffff>,<offset>.. (hardware write watchpoints
  on the function's first argument's data; reports each writing instruction).
- 120 / 144 fps, world rate (2026-11-04, reworked 2026-11-07; the timer part of `-RateMode world`): the 60 fps cap is the game's own. Its frame timer
  (object at [0x9b6250]: +8 rate, +0xc step, +0x10 cap rate, +0x14 minimum frame time) is set to 59.94
  once by the HMD config loader; level starts copy the rate into the cap; the main loop (sub_426e10)
  always runs at least one update of exactly one step per frame (up to ~4 to catch up), then waits out
  the cap. So an uncapped rate would fast-forward the game; the rate itself has to change. The emulator
  now does that at run time (SHADPS4_SSU_FPS, wait_stats.cpp SsuFollowGameRate, every 100 ms from the
  vblank loop), so any rate works and `headset` follows the refresh OpenXR measures (snapped to
  60/72/80/90/120/144). Nothing in the game caches the rate (analysis\timer_readers.py). The old patch
  `ssuvr-fps` (120 only) is now always written disabled.
  Frame-counted settings, rescaled with the rate (orig x rate/59.94 frames, factor^(59.94/rate)):
  hud multiplier_countdown_frames [0x9bdf0c] 30; lurker speed_per_frame_mul [0x9b96c4] 0.95;
  centipede / centipede_black head_scale_per_frame_mul [0x9b91c4] / [0x9b9524] 0.97.
  Tests 2026-11-07 (ssuvr-work\rate-test.ps1, rate-idle.ps1, rate-firemove.ps1, runs\20261107-04*):
  menus, level intro, enemy spawns, shot rate, shot speed and effects match 60 Hz; the first phase
  starts ~1 s sooner at 120/144 (a short frame-counted delay, harmless). One scripted 144 Hz game
  cleared the level in about half the usual time (one sample; power-up luck varies a lot). 400% scene
  on the monitor: 144 fps held through play (GPU ~53%; 60 fps ~28%). Not yet played in the headset.
- Colours (2026-11-05): until this build the headset picture was gamma-encoded twice: too bright and
  washed out, with lifted shadows, flat lighting and highlights, and banding in dark gradients such as
  the nebulae. The game's eye pictures are 8-bit UNORM textures that already hold display-ready (sRGB)
  values. The emulator took them for linear light, and the sRGB headset image encoded them again. Astro
  Bot's eye textures are sRGB, so the GPU decodes them when sampling and it never had the problem. The
  emulator now decodes UNORM eye pictures once (log line "Headset pictures: eye views R8G8B8A8Unorm,
  decoded from sRGB"). `SHADPS4_VR_EYE_DECODE=0` gives the old look for comparison, and `=1` forces
  decoding. The headset image is SDR whatever the Windows HDR setting. Before/after against the PS4:
  `C:\Games\ssuvr-work\hmdshots\gamma-fix\planet-ps4-fixed-before.png`.
- Shots (2026-11-07): the player's bolts (and other point sprites) were invisible at every render
  size. The game draws them as points and samples the sprite texture with the pixel's barycentric
  coordinates, which on the PS4 GPU run from 0 to 1 across a point. In Vulkan they are constant over
  a point, so every bolt showed its texture's black corner texel. Point draws now get the sprite
  coordinate (PointCoord) as their barycentrics. The shader cache version went up, so the first
  launch after this build recompiles shaders (brief hitches).
- Bloom (2026-11-06): at 400% the bloom pass read only every 16th scene pixel, so glow trails broke
  into strings of beads. The emulator now box-filters the scene down before the bloom reads it
  (`SHADPS4_DOWNSAMPLE_READS=0` turns that off, for comparison).
- Debug knobs for finding draws (env, off by default): `SHADPS4_LOG_PASSES_SHADERS=1`,
  `SHADPS4_DEBUG_SKIP=<hash,..>` (+ `SHADPS4_DEBUG_SKIP_RIGHT=1`), `SHADPS4_DEBUG_HIGHLIGHT=<hash>[:rgb],..`,
  `SHADPS4_DEBUG_NOALPHA=<hash,..>`, `SHADPS4_DEBUG_DESCRIBE=<hash>` (+ `_DUMP=<folder>`).
  `SHADPS4_DEBUG_SWEEP=<step s>,<start s>[,right][,alt][,learn=<s>][,wait=<s>][,until=<s>][,repeat=<n>][,freeze]` drops every
  pipeline in turn and captures each step, to find draws that cover pixels but show nothing. Paused frame: `,alt`, both eyes
  (`analysis\sweep2.py`). Gameplay: `,right,until=..,repeat=..` with `user\vr.json` = `{ "ipd_mm": 0, "fov_tan": [1.2, 1.2,
  1.262872, 1.262872] }` (eyes identical, so each right-eye drop is compared with the left eye of the same frame) and
  `ssuvr-work\script-circle.txt`; read with `analysis\sweep4.py`. `,freeze` stops the game world for still frames (explosions
  stop drawing then). Results: `ssuvr-work\shader-check-20261107.md`.
- Known: `user\sys_modules` holds the dumped
  system modules (libSceLibcInternal.sprx etc.) the NP toolkit needs; Steam shortcut "Super Stardust Ultra VR (PC VR port)" added 2026-11-01
  (runs "Play Super Stardust Ultra VR (Steam).bat", OpenVR flag set, no icon since the dump has no icon0.png).
