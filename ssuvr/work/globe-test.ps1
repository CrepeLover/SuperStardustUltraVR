# Globe-size test (2026-11-07, user: the globe the ship/shots/enemies travel on is about twice as big as
# it should be). Same fire-and-move script as the fm60 baseline (native scene, PSVR FOV), changing one
# thing each: globe400 = the 400% scene (as installed); globeFov = native scene with the Frame's FOV.
$ErrorActionPreference = 'Stop'
$work = 'C:\Games\ssuvr-work'
$common = @{ SDL_AUDIO_DRIVER = 'dummy'; SHADPS4_SHOT_SECONDS = '3,54' }
$runs = [ordered]@{
    'globe400' = @{ native = $false; env = @{} }
    'globeUser' = @{ native = $false; env = @{ SHADPS4_VR_HEADSET_FOV_TAN = '1.6881,1.2163,1.1684,1.7331'; SHADPS4_VR_FOV_OF = 'headset'; SHADPS4_SSU_FPS = '120' } }
    'globeFov' = @{ native = $true; env = @{ SHADPS4_VR_HEADSET_FOV_TAN = '1.6881,1.2163,1.1684,1.7331'; SHADPS4_VR_FOV_OF = 'headset' } }
}
foreach ($tag in $runs.Keys) {
    $e = $common.Clone(); foreach ($k in $runs[$tag].env.Keys) { $e[$k] = $runs[$tag].env[$k] }
    try {
        if ($runs[$tag].native) { & "$work\scene-native.ps1" | Out-Null }
        & "$work\smoke.ps1" -Mode virtual -Seconds 88 -Tag $tag -Script "$work\script-firemove.txt" -Shots '87' -Env $e *> "$work\runs\$tag-console.txt"
    } finally {
        if ($runs[$tag].native) { & "$work\scene-native.ps1" -Restore | Out-Null }
    }
    "$tag -> " + (Get-Content "$work\runs\$tag-console.txt" | Where-Object { $_ -like 'RESULT:*' } | Select-Object -First 1)
}
'done'
