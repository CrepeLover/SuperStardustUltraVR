# Super Stardust Ultra VR boost / travel test (2026-11-07): script-boost.txt (fire up 50-84 s; move right 66-69,
# right + l2 = boost 71-74); the SSU probe scans at 56 and 63 s (ship still) and watches to 84 s;
# then analysis\ship_arc.py for the arc and speed in 0.5 s steps. Muted, virtual headset. -Env adds knobs
# (SHADPS4_SSU_FPS = '120' for world rate 120).
param([string]$Tag = 'boost-60', [hashtable]$Env = @{}, [switch]$Deploy)
$ErrorActionPreference = 'Stop'
$work = 'C:\Games\ssuvr-work'
$moved = 'C:\Games\SuperStardustUltraVR\pc-vr\user\log\ssu-moved.csv'
$track = Join-Path (Split-Path $moved) 'ssu-track.csv'
$envs = @{ SDL_AUDIO_DRIVER = 'dummy'; SHADPS4_SSU_SCAN = '56,63,84' }
foreach ($k in $Env.Keys) { $envs[$k] = $Env[$k] }
try {
    & "$work\scene-native.ps1" | Out-Null
    foreach ($f in $moved, $track) { if (Test-Path $f) { Rename-Item $f ("old-" + [guid]::NewGuid().ToString('N') + '-' + (Split-Path $f -Leaf)) } }
    $args = @{ Mode = 'virtual'; Seconds = 90; Tag = $Tag; Script = "$work\script-boost.txt"; Shots = '65,72,77,82'; SkipVideo = $true; Env = $envs }
    if ($Deploy) { $args.Deploy = $true }
    & "$work\smoke.ps1" @args *> "$work\runs\$Tag-console.txt"
    $run = (Get-ChildItem "$work\runs" -Directory -Filter "*-virtual-$Tag" | Sort-Object Name | Select-Object -Last 1).FullName
    foreach ($f in $moved, $track) { if (Test-Path $f) { Move-Item $f (Join-Path $run (Split-Path $f -Leaf)) } }
    "run: $run"
    Select-String -Path "$run\shad_log.txt" -Pattern 'SSU probe' | Select-Object -First 6 | ForEach-Object { '  ' + $_.Line.Substring($_.Line.IndexOf('SSU probe')) }
    & C:\Users\<user>\envs\engineering\.venv\Scripts\python.exe -I "$work\analysis\ship_arc.py" $run
} finally {
    & "$work\scene-native.ps1" -Restore | Out-Null
}
