# A/B regression battery for Super Stardust Ultra VR (2026-11-10): runs the same scripted scenarios with the
# emulator currently installed in C:\Games\SuperStardustUltraVR\pc-vr (or -Deploy the build first) and writes
# runs\ab-<Tag>.txt with the numbers to compare between builds. Muted, virtual headset, no SteamVR.
#   1 w120  : script-circle at world rate 120, respawn shield on, bullet probe (shot lives / reach)
#   2 w60   : the same at 60
#   3 perf  : script-circle at the user's saved scene size (no scene-native), world rate 120, frame rate log
#   4 recenter: script-recenter.txt, scripted recenter must rewrite the game's calibrate offset
param([Parameter(Mandatory = $true)][string]$Tag, [switch]$Deploy, [string]$Only = '')
$ErrorActionPreference = 'Continue'
$work = 'C:\Games\ssuvr-work'; $runs = "$work\runs"; $py = 'C:\Users\<user>\envs\engineering\.venv\Scripts\python.exe'
$report = "$runs\ab-$Tag.txt"
$exe = 'C:\Games\SuperStardustUltraVR\pc-vr\shadps4.exe'
if ($Deploy) { Copy-Item -LiteralPath 'C:\Games\Farpoint-src\build\win-us\shadps4.exe' -Destination $exe -Force }
$sha = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash.Substring(0, 8)
"A/B battery '$Tag' with exe $sha, $(Get-Date -Format s)" | Set-Content $report
function Want([string]$n) { return (-not $Only) -or (($Only -split ',') -contains $n) }
function LastRun([string]$tag) { (Get-ChildItem $runs -Directory -Filter "*-virtual-$tag" | Sort-Object Name | Select-Object -Last 1).FullName }
function Facts([string]$run) {
    $log = Join-Path $run 'shad_log.txt'
    $out = @((Get-Content (Join-Path $run 'summary.txt') -TotalCount 1))
    if (Test-Path $log) {
        $l = Get-Content $log
        $out += "applied patches: " + @($l | Select-String 'Applied patch').Count
        $out += @($l | Select-String -Pattern 'per-update values rescaled|Headset pictures: eye views|Game frame rate|level start|Recenter|calibrat' | Select-Object -First 8 | ForEach-Object { '  ' + ($_.Line -replace '^\[[^\]]*\] ', '') })
        $fps = @($l | Select-String -Pattern 'headset frames so far, ([\d.]+) per second' -AllMatches | ForEach-Object { [double]$_.Matches[0].Groups[1].Value })
        if ($fps.Count) { $s = $fps | Measure-Object -Average -Minimum -Maximum; $out += ('fps (per 10 s window): n {0} avg {1:N1} min {2} max {3}' -f $s.Count, $s.Average, $s.Minimum, $s.Maximum) }
    }
    return $out
}
if (Want 'w120') {
    & "$work\bullet-test.ps1" -Fps 120 -Tag "$Tag-w120" -Env @{ SHADPS4_SSU_SET = '9be5bc=1000' } *> "$runs\$Tag-w120-wrapper.txt"
    $r = LastRun "$Tag-w120"; "`n== w120 ($r)" | Add-Content $report; Facts $r | Add-Content $report
    Get-Content "$runs\$Tag-w120-wrapper.txt" | Select-Object -Skip 2 | Add-Content $report
}
if (Want 'w60') {
    & "$work\bullet-test.ps1" -Tag "$Tag-w60" -Env @{ SHADPS4_SSU_SET = '9be5bc=1000' } *> "$runs\$Tag-w60-wrapper.txt"
    $r = LastRun "$Tag-w60"; "`n== w60 ($r)" | Add-Content $report; Facts $r | Add-Content $report
    Get-Content "$runs\$Tag-w60-wrapper.txt" | Select-Object -Skip 2 | Add-Content $report
}
if (Want 'perf') {
    & "$work\smoke.ps1" -Mode virtual -Seconds 140 -Tag "$Tag-perf" -Script "$work\script-circle.txt" -SkipVideo -Shots '100' `
        -Env @{ SDL_AUDIO_DRIVER = 'dummy'; SHADPS4_SSU_FPS = '120'; SHADPS4_SSU_DRAW_FPS = ''; SHADPS4_VR_REFRESH_RATE = '120'; SHADPS4_SSU_SET = '9be5bc=1000'; SHADPS4_SHOT_SECONDS = '30,95' } *> "$runs\$Tag-perf-console.txt"
    $r = LastRun "$Tag-perf"; "`n== perf ($r)" | Add-Content $report; Facts $r | Add-Content $report
    $shots = 'C:\Games\SuperStardustUltraVR\pc-vr\user\screenshots'
    if (Test-Path $shots) { New-Item -ItemType Directory -Force "$r\eyes" | Out-Null; Get-ChildItem $shots -File | Where-Object { $_.LastWriteTime -gt (Get-Item $r).CreationTime } | Move-Item -Destination "$r\eyes" }
    "eye captures: " + @(Get-ChildItem "$r\eyes" -File -ErrorAction SilentlyContinue).Count | Add-Content $report
}
if (Want 'recenter') {
    & "$work\smoke.ps1" -Mode virtual -Seconds 90 -Tag "$Tag-recenter" -Script "$work\script-recenter.txt" -SkipVideo -Shots '85' `
        -Env @{ SDL_AUDIO_DRIVER = 'dummy'; SHADPS4_SSU_SCAN = '60,64,88'; SHADPS4_SSU_SCAN_RANGES = '1.49-1.53' } *> "$runs\$Tag-recenter-console.txt"
    $r = LastRun "$Tag-recenter"; "`n== recenter ($r)" | Add-Content $report; Facts $r | Add-Content $report
}
"`ndone $(Get-Date -Format s)" | Add-Content $report
Get-Content $report
