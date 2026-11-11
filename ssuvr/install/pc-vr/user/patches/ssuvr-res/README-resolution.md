# Render resolution (scene and eye pictures)

Everything here is written by `pc-vr\launch-ssuvr.ps1` on every start from its "Render resolution"
setting (`-RenderPercent 100|150|200|250|300|400`, saved in `ssuvr-settings.json`). Do not edit the
files by hand; change the setting instead.

## How the game draws a frame (measured 2026-11-03)

Measured with the emulator's render-pass log (`SHADPS4_LOG_PASSES=5`, see the README):

1. All 3D is drawn into **one side-by-side buffer**: an R8G8B8A8 colour target with 2 colour
   fragments plus a D32S8 depth target with 8 samples (EQAA). Originally it is 1920x1080, with
   viewports 0,0 960x1080 (left eye) and 960,0 960x1080 (right eye): the PSVR's 960x1080 an eye.
   A single-sample resolve target of the same size follows.
2. A bloom chain at 480x540, 240x270, 120x135 and 60x67.
3. One full-screen draw per eye composites the resolved scene and the bloom into the **eye
   pictures**, which go to the headset.

So the eye pictures alone only control how big the final picture is. Before 2026-11-03 the launcher
changed only those, which stretched a 960x1080 scene up to 3840x4320: that was the "really low
internal resolution, blown up to 4K" seen in the headset. (The older notes here said the scene was
drawn in the eye targets and that Plan18.HMDconfig did not matter; both were wrong. Shrinking the eye
targets made the picture blocky only because the final picture got small.)

## What the launcher changes

- **Eye pictures**: `CUSA05951.xml` in this folder patches the packed constant the four eye views are
  built from (`0x55782e: movabs rax, 0x438000003c0` = 960 | 1080 << 32, view constructor 0x41cf40;
  patch address 0x957830 = vaddr 0x557830 + 0x400000) to the chosen size, e.g. 3840x4320 at 400%.
- **Scene**: the side-by-side buffer is sized by the game's own config file
  `games\CUSA05951\Plan18.HMDconfig` (`DISPLAY_SCREEN_RESOLUTION` = both eyes, `WINDOW_DIMENSIONS
  0 <w> 0 <h> 0` = one eye; parsed by sub_25b710). The launcher rewrites those two lines from the
  untouched copy `pc-vr\Plan18.HMDconfig.original` so the scene matches the eye pictures, e.g.
  `7680 4320` / `0 3840 0 4320 0` at 400%. At 100% the file is byte-for-byte the original.
- **GPU heap (400% only)**: engine init (sub_555a30) allocates one fixed "Shared Video" heap of
  0xA6800000 bytes (2.66 GB) that the scene targets, mesh blocks and the streamer share. A 7680x4320
  scene with 8 depth samples exhausts it (the game asserts in Mesh_Block_Manager_PS4.cpp line 66 and
  Streamer_Impl.cpp line 731, then crashes). `..\ssuvr-heap\CUSA05951.xml` raises it to 0xF0000000
  (3.75 GB; the size is a 32-bit field stored at 0x9dddf4) at four sites: 0x555bfb (allocate),
  0x555c2c (map), 0x555cb2 (the "Shared Video size" print) and 0x555ed3 (stored size). Left alone
  on purpose: `mov ecx, 0xa6800000` at 0x555ce6, which only builds the mask 0xFFFFFFFF with the add
  after it. The launcher also sets `extra_dmem_in_mbytes = 2048` in `user\config.json` (direct memory
  4.5 GB -> 6.5 GB) so that allocation fits; extra memory alone does not help, the game's heap size
  is a constant. Below 400% the heap patch is off and the extra memory is 0.

The 1920x1080 constant at 0x556899 belongs to the TV output view ("frontbuffer") and does not
matter for the headset. The HMD library's reported panel size (`SHADPS4_HMD_PANEL`) is ignored.

## Verified (2026-11-03, monitor-only runs with the virtual headset)

| Setting | Scene buffer (an eye) | Result |
| --- | --- | --- |
| 100% (original) | 1920x1080 (960x1080) | 60 fps |
| 200% | 3840x2160 (1920x2160) | 60 fps |
| 300% | 5760x3240 (2880x3240) | 60 fps at the title, eye captures correct and sharp |
| 400% without the heap patch | 7680x4320 (3840x4320) | crashes (game heap exhausted), also with 2 GB extra memory |
| 400% with heap patch + 2 GB extra memory | 7680x4320 (3840x4320) | 57-60 fps, no asserts, eye captures correct and sharp at native pixels; also verified started through the launcher |

Full-resolution eye captures without a display attached: put a number N in `user\auto_shot_every`
(a picture of each eye every N presents, into `user\screenshots`). Captures cost a lot of frame
rate; remove the file afterwards.

## Still worth knowing

- SteamVR resamples the eye pictures to its own render target for the Steam Frame and streams them;
  that is the last step before the panels (compositor stereo captures: C:\Games\ssuvr-work\hmdshots\
  hmdshot.py).
- The scene still uses the game's EQAA (8 depth samples, 2 colour fragments). At 4K an eye that is
  mostly wasted GPU time and memory; making those targets single-sampled would free both.
