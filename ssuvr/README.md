# Super Stardust Ultra VR on PC VR

A PC VR port of Super Stardust Ultra VR (CUSA05951, EU 1.03). It runs on shadPS4 with evertec82's Farpoint OpenXR fork (https://github.com/evertec82/Farpoint). The emulator changes are on this branch (`ssuvr`). They're described in the commit "Super Stardust Ultra VR (CUSA05951) on the Farpoint PC VR emulator". shadPS4 main of 2026-11-10 is merged in on top.

This folder holds the parts that live outside the emulator source:

- `install/`: the launcher (`pc-vr/launch-ssuvr.ps1`, with a settings window), the default emulator config, memory patches (`pc-vr/user/patches/*`), the batch files the Steam shortcut runs, `README-SSUVR.md`, the full notes on settings, the 120 Hz world rate, weapons and known issues, and `pc-vr/BUILD.txt`, the local builds with their SHA-256. No game files.
- `work/`: the build script (`build-ssuvr.cmd`), the test harness (`smoke.ps1` plus the `*-test.ps1` scripts, scripted input `script-*.txt`, and `ab-ssuvr.ps1`, which runs the same scenarios on two builds for comparison), analysis tools (`analysis/`, which need the capstone disassembler and data you extract from your own copy of the game), and Steam shortcut helpers (`steam/`).

The scripts expect this layout:

| Folder | Contents |
|---|---|
| `C:\Games\SuperStardustUltraVR` | the install |
| `C:\Games\Farpoint-src` | this repository |
| `C:\Games\ssuvr-work` | the work folder |

Placeholders like `C:\Users\<user>`, `<steam-user-id>`, `<share>` and `<lan-ip>` replace local details.
