# Super Stardust Ultra VR draw-rate test (2026-11-07): fires up 50-66 s with the ship still (script-firelong.txt),
# traces the shots 53-64 s (as the 60 Hz reference 20261107-144332-virtual-firelong-60: reach 60.07 deg, life
# 0.72 s), window shots to catch black or late frames, then analysis\shot_range.py. Muted, virtual headset.
# -DrawFps 120 sets SHADPS4_SSU_DRAW_FPS; -Once sets SHADPS4_SSU_DRAW_ONCE; -Env adds more.
param([string]$DrawFps = '120', [string]$Once = '', [string]$Tag = '', [switch]$Deploy, [hashtable]$Env = @{},
      [string]$Script = 'C:\Games\ssuvr-work\script-firelong.txt', [string]$Shots = '30,40,50,56,58,60,62,64')
$ErrorActionPreference = 'Stop'
$work = 'C:\Games\ssuvr-work'; $log = 'C:\Games\SuperStardustUltraVR\pc-vr\user\log'
if (-not $Tag) { $Tag = "draw$DrawFps" + $(if ($Once) { "-once$Once" } else { '' }) }
$envs = @{ SDL_AUDIO_DRIVER = 'dummy'; SHADPS4_SSU_TRACE = '53,64'; SHADPS4_SSU_DRAW_FPS = $DrawFps; SHADPS4_SSU_DRAW_ONCE = $Once }
foreach ($k in $Env.Keys) { $envs[$k] = $Env[$k] }
& "$work\scene-native.ps1" | Out-Null
try {
    foreach ($name in 'ssu-trace.bin', 'ssu-trace-addr.txt') { if (Test-Path "$log\$name") { Rename-Item "$log\$name" ("old-" + [guid]::NewGuid().ToString('N') + "-$name") } }
    $args = @{ Mode = 'virtual'; Seconds = 80; Tag = $Tag; Script = $Script; SkipVideo = $true; Shots = $Shots; Env = $envs }
    if ($Deploy) { $args.Deploy = $true }
    & "$work\smoke.ps1" @args *> "$work\runs\$Tag-console.txt"
    $run = (Get-ChildItem "$work\runs" -Directory -Filter "*-virtual-$Tag" | Sort-Object Name | Select-Object -Last 1).FullName
    foreach ($name in 'ssu-trace.bin', 'ssu-trace-addr.txt') { if (Test-Path "$log\$name") { Move-Item "$log\$name" "$run\$name" } }
    "run: $run"
    & C:\Users\<user>\envs\engineering\.venv\Scripts\python.exe -I "$work\analysis\shot_range.py" $run
    # Mean brightness of each window shot (black frames show as ~0).
    Add-Type -AssemblyName System.Drawing
    $line = foreach ($f in Get-ChildItem $run -Filter 'shot-*.png' | Sort-Object { [int]($_.BaseName -replace 'shot-', '') }) {
        $b = [System.Drawing.Bitmap]::FromFile($f.FullName); $sum = 0; $n = 0
        for ($y = 60; $y -lt $b.Height; $y += 8) { for ($x = 0; $x -lt $b.Width; $x += 8) { $c = $b.GetPixel($x, $y); $sum += $c.R + $c.G + $c.B; $n++ } }
        $b.Dispose(); '{0}:{1:N0}' -f ($f.BaseName -replace 'shot-', ''), ($sum / $n / 3)
    }
    'window brightness by second: ' + ($line -join ' ')
    Select-String -Path "$run\shad_log.txt" -Pattern 'Draw rate:|headset frames so far' | Select-Object -First 1 -Last 2 | ForEach-Object { $l = $_.Line; $l.Substring([Math]::Max(0, $l.IndexOf(': ') + 2)).Substring(0, [Math]::Min(220, $l.Length - $l.IndexOf(': ') - 2)) }
} finally { & "$work\scene-native.ps1" -Restore | Out-Null }
