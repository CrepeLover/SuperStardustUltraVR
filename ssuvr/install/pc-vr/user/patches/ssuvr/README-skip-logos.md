# Boot straight to the title screen (Super Stardust Ultra VR 1.03, CUSA05951)

Loaded automatically by the emulator from user\patches\<folder>\files.json + CUSA05951.xml; the log shows
"Applied patch: Boot straight to the title screen" six times. XML addresses are eboot vaddr + 0x400000.
Disable with isEnabled="false" in the XML (then put the Cross presses back into pc-vr\skip-intro.txt).

| vaddr    | original                 | patched                  | effect |
|----------|--------------------------|--------------------------|--------|
| 0x383a5d | 31 c9 4c 89 f7           | b9 06 00 00 00           | make_startup_chain: first Startup_Fader page = 6 (legal text) instead of 0; pages 0-5 (autosave notice, SIE, XDEV, Housemarque, d3t video) are never built |
| 0x4ede90 | 75 12                    | 90 90                    | Startup_Fader::on_exit: set_save_allowed(true) on exit of any page (page 0 no longer exists) |
| 0x4ed8ae | c5 fa 10 05 de 0a 4e 00  | c5 f8 57 c0 90 90 90 90  | Startup_Fader::update: mode-0 show_time = 0, pages expire on their first update (Read-me viewer is mode 1, untouched) |
| 0x4ed91d | 0f 84 da 01 00 00        | 90 x6                    | page 6 no longer waits for Cross (part 1) |
| 0x4ed92a | 0f 85 cd 01 00 00        | 90 x6                    | page 6 no longer waits for Cross (part 2) |
| 0x50ad09 | 80 bb dc 00 00 00 00 0f 84 06 01 00 00 83 bb a8 00 00 00 08 0f 85 f9 00 00 00 | 80 bb dd 00 00 00 00 0f 84 06 01 00 00 80 bb 44 00 00 00 00 0f 85 f9 00 00 00 | Morpheus_Calibration_Screen::update: the Cross path runs by itself once, on the second update |

Verified 2026-11-01 on the monitor (virtual headset): black until ~12 s, title screen at ~13 s, flat and
centred, no logo, legal or calibrate frame visible; main menu normal; the save file
(user\home\1000\savedata\CUSA05951\sce_sdmemory\memory.dat) is written at ~12 s.

Why the chain starts at page 6 and not 7: starting at 7 (no legal page) reaches the calibrate screen before
the menu view has swung into place, so the stored view offset is wrong and the title/menus sit on a panel
tilted about 20 degrees. Going through the (instantly expired) legal page keeps the original ordering. The
same tilt appears with a real Cross press on the calibrate screen when the chain starts at 7, so it is not
caused by the auto-press.
