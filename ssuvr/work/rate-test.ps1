# Game frame rate test (2026-11-07): does the game logic hold at 120 / 144 Hz (SHADPS4_SSU_FPS), and does
# the GPU keep up at the 400% scene? Logic runs: native scene, identical eyes (vr.json), the same live
# right-eye sweep as the 60 Hz baseline run 20261107-035401-virtual-sweep6-repeat. Fps runs: 400% scene,
# no sweep, no eye captures. Always restores the 400% scene and moves vr.json away.
# Muted (SDL_AUDIO_DRIVER=dummy). Usage: rate-test.ps1 [-Only logic|fps]
param([ValidateSet('all','logic','fps')][string]$Only = 'all')
$ErrorActionPreference = 'Stop'
$work = 'C:\Games\ssuvr-work'
$vr = 'C:\Games\SuperStardustUltraVR\pc-vr\user\vr.json'
$script = "$work\script-circle.txt"
$shots = (10..33 | ForEach-Object { $_ * 5 }) -join ','
$sweep = '0.6,48,right,learn=4,wait=3,repeat=8,until=97'
$common = @{ SDL_AUDIO_DRIVER = 'dummy' }
function Run($tag, [hashtable]$envs, [string]$shotList) {
    $e = $common.Clone(); foreach ($k in $envs.Keys) { $e[$k] = $envs[$k] }
    & "$work\smoke.ps1" -Mode virtual -Seconds 165 -Tag $tag -Script $script -Shots $shotList -Env $e *> "$work\runs\$tag-console.txt"
    $line = Get-Content "$work\runs\$tag-console.txt" | Where-Object { $_ -like 'RESULT:*' } | Select-Object -First 1
    "$tag -> $line"
}
try {
    if ($Only -in 'all','logic') {
        if (Test-Path $vr) { throw 'vr.json exists' }
        Set-Content -Path $vr -Value '{ "ipd_mm": 0, "fov_tan": [1.2, 1.2, 1.262872, 1.262872] }' -Encoding ascii
        & "$work\scene-native.ps1" | Out-Null
        Run 'rate120-logic' @{ SHADPS4_SSU_FPS = '120'; SHADPS4_DEBUG_SWEEP = $sweep; SHADPS4_LOG_PASSES = '5'; SHADPS4_LOG_PASSES_SHADERS = '1' } $shots
        Run 'rate144-logic' @{ SHADPS4_SSU_FPS = 'headset'; SHADPS4_VR_REFRESH_RATE = '144'; SHADPS4_DEBUG_SWEEP = $sweep; SHADPS4_LOG_PASSES = '5'; SHADPS4_LOG_PASSES_SHADERS = '1' } $shots
    }
} finally {
    & "$work\scene-native.ps1" -Restore | Out-Null
    if (Test-Path $vr) { Move-Item -LiteralPath $vr -Destination "$vr.symfov-test" -Force }
}
if ($Only -in 'all','fps') {
    Run 'rate144-400' @{ SHADPS4_SSU_FPS = 'headset'; SHADPS4_VR_REFRESH_RATE = '144' } '100,150'
    Run 'rate120-400' @{ SHADPS4_SSU_FPS = '120' } '100,150'
}
'done'
