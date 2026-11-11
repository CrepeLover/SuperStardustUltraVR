<!--
SPDX-FileCopyrightText: 2026 CrepeLover
SPDX-License-Identifier: GPL-2.0-or-later
-->

# Super Stardust Ultra VR on PC VR

**Super Stardust Ultra VR (PS4 / PlayStation VR) on a PC VR headset**, played from your own copy of
the game through a PS4 emulator. It also plays flat on the monitor with a gamepad.

The emulator is [shadPS4](https://github.com/shadps4-emu/shadPS4) with
[evertec82's Farpoint PC VR port](https://github.com/evertec82/Farpoint), whose OpenXR bridge comes
from [bigmak94's AstroQuest](https://github.com/bigmak94/AstroQuest). This repository adds what
Super Stardust Ultra VR needs, on Farpoint v0.9.6 with shadPS4 main of 2026-11-10 merged in. It was
developed and played on a Steam Frame through SteamVR.

## What it adds

- **Shots you can see.** The game draws its shots as point sprites. Their pixel shaders now get the
  sprite coordinate (PointCoord) where the GPU hands it over, which fixes invisible player and enemy
  shots.
- **The game's own colours in the headset.** Eye pictures that hold display-encoded bytes are
  decoded from sRGB once, not twice.
- **120 Hz without changing the game.** The world can update at the headset's rate (120 or 144 Hz)
  with its per-update values rescaled, so ships, shots and particles move as they do at 60.
- **Resolution and field of view.** The launcher sizes the game's 3D scene and its eye pictures
  together, up to 400% (3840x4320 an eye), and tells the game the headset's own field of view.
- **Recentering.** SteamVR's view reset recalibrates the game, as its own calibrate option does.
- **Straight to the title screen.** A memory patch skips the logos, the intro video and the
  calibrate prompt.
- **Steam Frame controllers** in DualShock positions, 16x anisotropic filtering, and MLAA on the eye
  pictures (shared with the Farpoint build).
- Generic emulator fixes found with this game: the PlayStation VR tracker parameters of SDK 3.5
  titles, `sceVideoOutRegisterStereoBuffers`, packed-stereo detection, a trophy context without a
  trophy pack, and box-filtered reads of big textures by small targets (bloom).

## What you need

- A Windows PC. For VR, a PC VR headset with its OpenXR runtime (SteamVR, Virtual Desktop or the
  vendor's own).
- Your own copy of the game, CUSA05951 (EU): base 1.00 with patch 1.03 merged in. No game files are
  included here.
- PS4 system modules from your own console in `pc-vr\user\sys_modules`, the same set as AstroQuest's
  PC build: libSceLibcInternal, libSceNgs2, libSceRtc, libSceFont, libSceFontFt, libSceFreeTypeOt,
  libSceJpegDec, libSceJpegEnc and libScePngEnc (`.sprx`).
- An emulator built from this repository (see Building).

## Install and play

1. Copy `ssuvr/install/` to `C:\Games\SuperStardustUltraVR\`, put the game in
   `games\CUSA05951\` there and `shadps4.exe` in `pc-vr\`.
2. Start the OpenXR runtime, then run `Play Super Stardust Ultra VR.bat`. A settings window opens
   (headset or flat, frame rate, resolution, field of view and more); press Start.
3. To start from Steam or SteamVR, add `Play Super Stardust Ultra VR (Steam).bat` as a non-Steam
   game. It uses the settings last saved in the window.

[`ssuvr/install/README-SSUVR.md`](ssuvr/install/README-SSUVR.md) has the full notes: every setting
and launcher option, the 120 Hz world rate, resolution, recentering and known issues.

## Building

As for evertec82's Farpoint port ([BUILD-FARPOINT.md](BUILD-FARPOINT.md)): a Windows build of
shadPS4 with `-DENABLE_OPENXR=ON`. [`ssuvr/work/build-ssuvr.cmd`](ssuvr/work/build-ssuvr.cmd) is the
script used here (Visual Studio 2022, clang-cl from LLVM 23, CMake and Ninja, at idle priority). The
rest of [`ssuvr/work/`](ssuvr/work) is the test harness (scripted input on a virtual headset, A/B
comparisons between builds) and analysis tools.

## License

GPL-2.0-or-later, as shadPS4 ([LICENSE](LICENSE)). This project is not affiliated with Sony
Interactive Entertainment or the game's developers.
