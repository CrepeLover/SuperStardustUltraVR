# Flat vs VR (virtual headset) with the same input: fire 50-66 s, move right 66-70 s (2026-11-07, user: compare
# the arc the ship and the Rock Crusher shots cover in the two modes). Window captures at the same script times.
$ErrorActionPreference = 'Stop'
$work = 'C:\Games\ssuvr-work'
$shots = '60,63,65,66,67,68,69,70,71,73,76'
try {
    & "$work\scene-native.ps1" | Out-Null
    & "$work\smoke.ps1" -Mode flat -Seconds 80 -Tag modes-flat -Script "$work\script-flat-firemove2.txt" -Shots $shots -Env @{ SDL_AUDIO_DRIVER = 'dummy' } *> "$work\runs\modes-flat-console.txt"
    & "$work\smoke.ps1" -Mode virtual -Seconds 80 -Tag modes-vr -Script "$work\script-firemove2.txt" -Shots $shots -Env @{ SDL_AUDIO_DRIVER = 'dummy'; SHADPS4_SHOT_SECONDS = '1,52' } *> "$work\runs\modes-vr-console.txt"
} finally {
    & "$work\scene-native.ps1" -Restore | Out-Null
}
'done'
