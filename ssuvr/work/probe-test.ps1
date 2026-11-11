# Ship arc length, flat vs VR (2026-11-07): same scripted input (fire 50-66 s, move right 66-70 s); the
# emulator's SSU probe scans memory at 56 and 63 s (ship still), watches until 78 s and writes the
# position vectors that moved (pc-vr\user\log\ssu-moved.csv, copied into the run folder).
param([string[]]$Modes = @('flat', 'virtual'), [switch]$Deploy)
$ErrorActionPreference = 'Stop'
$work = 'C:\Games\ssuvr-work'
$moved = 'C:\Games\SuperStardustUltraVR\pc-vr\user\log\ssu-moved.csv'
$first = $true
try {
    & "$work\scene-native.ps1" | Out-Null
    foreach ($mode in $Modes) {
        if (Test-Path $moved) { Rename-Item $moved ("ssu-moved-old-" + [guid]::NewGuid().ToString('N') + '.csv') }
        $script = if ($mode -eq 'flat') { "$work\script-flat-firemove2.txt" } else { "$work\script-firemove2.txt" }
        $args = @{ Mode = $mode; Seconds = 84; Tag = "probe-$mode"; Script = $script; Shots = '62,72'
                   Env = @{ SDL_AUDIO_DRIVER = 'dummy'; SHADPS4_SSU_SCAN = '56,63,78' } }
        if ($Deploy -and $first) { $args.Deploy = $true }
        $first = $false
        & "$work\smoke.ps1" @args *> "$work\runs\probe-$mode-console.txt"
        $run = (Get-ChildItem "$work\runs" -Directory -Filter "*-$mode-probe-$mode" | Sort-Object Name | Select-Object -Last 1).FullName
        if (Test-Path $moved) { Copy-Item $moved "$run\ssu-moved.csv" }; $track = Join-Path (Split-Path $moved) 'ssu-track.csv'; if (Test-Path $track) { Copy-Item $track "$run\ssu-track.csv"; Rename-Item $track ('ssu-track-old-' + [guid]::NewGuid().ToString('N') + '.csv') }
        "$mode -> $run"
        Select-String -Path "$run\shad_log.txt" -Pattern 'SSU probe' | ForEach-Object { '  ' + $_.Line.Substring($_.Line.IndexOf('SSU probe')) }
    }
} finally {
    & "$work\scene-native.ps1" -Restore | Out-Null
}
