@echo off
rem Starts Super Stardust Ultra VR with the saved settings and no settings window (for launching from Steam / SteamVR in the headset).
rem Run "Play Super Stardust Ultra VR.bat" on the desktop to change the headset mode, mirror or FOV; its choices are saved.
powershell.exe -NoProfile -STA -ExecutionPolicy Bypass -File "%~dp0pc-vr\launch-ssuvr.ps1" -NoGui %*
