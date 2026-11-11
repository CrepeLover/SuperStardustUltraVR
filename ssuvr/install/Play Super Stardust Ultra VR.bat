@echo off
rem Starts Super Stardust Ultra VR (CUSA05951) on PC VR with the settings window. Extra arguments go to pc-vr\launch-ssuvr.ps1.
powershell.exe -NoProfile -STA -ExecutionPolicy Bypass -File "%~dp0pc-vr\launch-ssuvr.ps1" %*
if errorlevel 1 pause
