# Super Stardust Ultra VR bullet test (2026-11-08): plays script-circle.txt (Arcade, circle and fire; enemies shoot
# back) at native scene size with the bullet probe on (SHADPS4_SSU_BULLETS=1 + SSU_COUNT=26f920), moves
# user\log\ssu-bullets.csv into the run, then analysis\bullet_lives.py. Muted, virtual headset.
# -Fps 120 sets SHADPS4_SSU_FPS (world rate); '' = 60. -Env adds more.
param([string]$Fps = '', [string]$Tag = '', [switch]$Deploy, [hashtable]$Env = @{}, [int]$Seconds = 150,
      [string]$Script = 'C:\Games\ssuvr-work\script-circle.txt')
$ErrorActionPreference = 'Stop'
$work = 'C:\Games\ssuvr-work'; $log = 'C:\Games\SuperStardustUltraVR\pc-vr\user\log'
if (-not $Tag) { $Tag = 'bullets-' + $(if ($Fps) { "w$Fps" } else { '60' }) }
$envs = @{ SDL_AUDIO_DRIVER = 'dummy'; SHADPS4_SSU_TRACE = ''; SHADPS4_SSU_COUNT = '26f920'; SHADPS4_SSU_BULLETS = '1';
           SHADPS4_SSU_FPS = $Fps; SHADPS4_SSU_DRAW_FPS = '' }
foreach ($k in $Env.Keys) { $envs[$k] = $Env[$k] }
& "$work\scene-native.ps1" | Out-Null
try {
    if (Test-Path "$log\ssu-bullets.csv") { Rename-Item "$log\ssu-bullets.csv" ("old-" + [guid]::NewGuid().ToString('N') + "-ssu-bullets.csv") }
    $args = @{ Mode = 'virtual'; Seconds = $Seconds; Tag = $Tag; Script = $Script; SkipVideo = $true; Shots = ''; Env = $envs }
    if ($Deploy) { $args.Deploy = $true }
    & "$work\smoke.ps1" @args *> "$work\runs\$Tag-console.txt"
    $run = (Get-ChildItem "$work\runs" -Directory -Filter "*-virtual-$Tag" | Sort-Object Name | Select-Object -Last 1).FullName
    if (Test-Path "$log\ssu-bullets.csv") { Move-Item "$log\ssu-bullets.csv" "$run\ssu-bullets.csv" }
    "run: $run"
    Get-Content "$run\summary.txt" -TotalCount 1
    if (Test-Path "$work\analysis\bullet_lives.py") { & C:\Users\<user>\envs\engineering\.venv\Scripts\python.exe -I "$work\analysis\bullet_lives.py" $run }
} finally { & "$work\scene-native.ps1" -Restore | Out-Null }
