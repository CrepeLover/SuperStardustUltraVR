# Logic-speed test (2026-11-07): the level with no input (script-idle.txt), native scene, an eye capture
# every 2 s from 50 s. With time-based game logic the same capture time shows the same scene at every
# game frame rate. 60 runs twice (is the game repeatable?), then 120 and 144 (SHADPS4_SSU_FPS).
$ErrorActionPreference = 'Stop'
$work = 'C:\Games\ssuvr-work'
$common = @{ SDL_AUDIO_DRIVER = 'dummy'; SHADPS4_SHOT_SECONDS = '2,50' }
$runs = [ordered]@{
    'idle60a' = @{}
    'idle60b' = @{}
    'idle120' = @{ SHADPS4_SSU_FPS = '120' }
    'idle144' = @{ SHADPS4_SSU_FPS = 'headset'; SHADPS4_VR_REFRESH_RATE = '144' }
}
try {
    & "$work\scene-native.ps1" | Out-Null
    foreach ($tag in $runs.Keys) {
        $e = $common.Clone(); foreach ($k in $runs[$tag].Keys) { $e[$k] = $runs[$tag][$k] }
        & "$work\smoke.ps1" -Mode virtual -Seconds 80 -Tag $tag -Script "$work\script-idle.txt" -Shots '75' -Env $e *> "$work\runs\$tag-console.txt"
        "$tag -> " + (Get-Content "$work\runs\$tag-console.txt" | Where-Object { $_ -like 'RESULT:*' } | Select-Object -First 1)
    }
} finally {
    & "$work\scene-native.ps1" -Restore | Out-Null
}
'done'
