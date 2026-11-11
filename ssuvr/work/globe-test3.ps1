# Globe-size test 3 (2026-11-07): native vs 400% scene (both PSVR FOV, 60 fps), move at 66-70 s script time
# so both are well into the level; compare where the planet comes to rest.
$ErrorActionPreference = 'Stop'
$work = 'C:\Games\ssuvr-work'
$common = @{ SDL_AUDIO_DRIVER = 'dummy'; SHADPS4_SHOT_SECONDS = '4,73' }
$runs = [ordered]@{ 'g4native' = @{ native = $true; env = @{} }; 'g4scene400' = @{ native = $false; env = @{} }; 'g4user' = @{ native = $false; env = @{ SHADPS4_VR_HEADSET_FOV_TAN = '1.6881,1.2163,1.1684,1.7331'; SHADPS4_VR_FOV_OF = 'headset'; SHADPS4_SSU_FPS = '120' } } }
foreach ($tag in $runs.Keys) {
    $e = $common.Clone(); foreach ($k in $runs[$tag].env.Keys) { $e[$k] = $runs[$tag].env[$k] }
    try {
        if ($runs[$tag].native) { & "$work\scene-native.ps1" | Out-Null }
        & "$work\smoke.ps1" -Mode virtual -Seconds 92 -Tag $tag -Script "$work\script-firemove2.txt" -Shots '91' -Env $e *> "$work\runs\$tag-console.txt"
    } finally {
        if ($runs[$tag].native) { & "$work\scene-native.ps1" -Restore | Out-Null }
    }
    "$tag -> " + (Get-Content "$work\runs\$tag-console.txt" | Where-Object { $_ -like 'RESULT:*' } | Select-Object -First 1)
}
'done'
