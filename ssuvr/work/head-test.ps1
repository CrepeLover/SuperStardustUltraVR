# Eye separation and head-movement scale in game units (2026-11-07): virtual headset, ship still and
# firing, head moved 10 cm to the right at 62 s (script-headmove.txt). The SSU probe scans at 56 and 60 s
# for stationary vectors of camera-distance length (265-335), dumps them all (ssu-stable.bin) and records
# the ones that move by 70 s (ssu-moved.csv, ssu-track.csv). analysis\eye_scale.py reads them.
param([switch]$Deploy, [string]$Tag = 'head', [int]$WorldSize = 100, [string]$ScriptFile = 'script-headmove.txt', [string]$Scan = '56,60,70', [int]$Seconds = 76, [string]$Shots = '58,66', [string]$Ranges = '265-335')
$ErrorActionPreference = 'Stop'
$work = 'C:\Games\ssuvr-work'
$log = 'C:\Games\SuperStardustUltraVR\pc-vr\user\log'
$names = 'ssu-moved.csv', 'ssu-track.csv', 'ssu-stable.bin'
try {
    & "$work\scene-native.ps1" | Out-Null
    foreach ($name in $names) {
        if (Test-Path "$log\$name") { Rename-Item "$log\$name" ("old-" + [guid]::NewGuid().ToString('N') + "-$name") }
    }
    $env = @{ SDL_AUDIO_DRIVER = 'dummy'; SHADPS4_SSU_SCAN = $Scan; SHADPS4_SSU_SCAN_RANGES = $Ranges; SHADPS4_SSU_SCAN_DUMP = '1'; SHADPS4_VR_WORLD_SIZE = $(if ($WorldSize -eq 100) { '' } else { [string]$WorldSize }) }
    $args = @{ Mode = 'virtual'; Seconds = $Seconds; Tag = $Tag; Script = "$work\$ScriptFile"; Shots = $Shots; Env = $env }
    if ($Deploy) { $args.Deploy = $true }
    & "$work\smoke.ps1" @args *> "$work\runs\$Tag-console.txt"
    $run = (Get-ChildItem "$work\runs" -Directory -Filter "*-virtual-$Tag" | Sort-Object Name | Select-Object -Last 1).FullName
    foreach ($name in $names) { if (Test-Path "$log\$name") { Move-Item "$log\$name" "$run\$name" } }
    $run
    Get-ChildItem $run -Filter 'ssu-*' | ForEach-Object { '  ' + $_.Name + ' ' + $_.Length }
} finally {
    & "$work\scene-native.ps1" -Restore | Out-Null
}
