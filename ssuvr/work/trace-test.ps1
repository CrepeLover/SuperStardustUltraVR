# Rock Crusher shot range, flat vs VR (2026-11-07): same scripted input (fire up 50-66 s with the ship
# still); the emulator's SSU trace scans memory three times from 53 s and then records every change of
# the positions that came and went until 64 s (pc-vr\user\log\ssu-trace.bin + ssu-trace-addr.txt, copied
# into the run folder). analysis\shot_range.py measures the shots.
param([string[]]$Modes = @('flat', 'virtual'), [switch]$Deploy, [string]$FlatTrace = '62,68', [string]$VrTrace = '53,59', [string]$Span = '200000000-300000000', [string]$Fps = '', [string]$DrawFps = '', [string]$TagSuffix = '')
$ErrorActionPreference = 'Stop'
$work = 'C:\Games\ssuvr-work'
$log = 'C:\Games\SuperStardustUltraVR\pc-vr\user\log'
$first = $true
try {
    & "$work\scene-native.ps1" | Out-Null
    foreach ($mode in $Modes) {
        foreach ($name in 'ssu-trace.bin', 'ssu-trace-addr.txt') {
            if (Test-Path "$log\$name") { Rename-Item "$log\$name" ("old-" + [guid]::NewGuid().ToString('N') + "-$name") }
        }
        $script = if ($mode -eq 'flat') { "$work\script-flat-firelong.txt" } else { "$work\script-firelong.txt" }
        $args = @{ Mode = $mode; Seconds = 84; Tag = "trace-$mode$TagSuffix"; Script = $script; Shots = '54,56,58,60,62,64,66,68,70'
                   Env = @{ SDL_AUDIO_DRIVER = 'dummy'; SHADPS4_SSU_TRACE = $(if ($mode -eq 'flat') { $FlatTrace } else { $VrTrace }); SHADPS4_SSU_TRACE_SPAN = $Span; SHADPS4_SSU_TRACE_SCANS = '2'; SHADPS4_SSU_FPS = $Fps; SHADPS4_SSU_DRAW_FPS = $DrawFps } }
        if ($Deploy -and $first) { $args.Deploy = $true }
        $first = $false
        & "$work\smoke.ps1" @args *> "$work\runs\trace-$mode-console.txt"
        $run = (Get-ChildItem "$work\runs" -Directory -Filter "*-$mode-trace-$mode$TagSuffix" | Sort-Object Name | Select-Object -Last 1).FullName
        foreach ($name in 'ssu-trace.bin', 'ssu-trace-addr.txt') {
            if (Test-Path "$log\$name") { Move-Item "$log\$name" "$run\$name" }
        }
        "$mode -> $run"
        Get-ChildItem $run -Filter 'ssu-trace*' | ForEach-Object { '  ' + $_.Name + ' ' + $_.Length }
    }
} finally {
    & "$work\scene-native.ps1" -Restore | Out-Null
}
