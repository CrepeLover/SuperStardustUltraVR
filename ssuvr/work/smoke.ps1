# Smoke-tests Super Stardust Ultra VR in the PC VR emulator on the monitor and summarizes the log.
# usage: smoke.ps1 [-Mode virtual|flat|xr] [-Seconds 90] [-Deploy] [-Tag name] [-Env @{NAME='value'}]
#   virtual = virtual headset, no OpenXR (SHADPS4_VR=1, SHADPS4_OPENXR=0): the VR code path on the monitor
#   flat    = no headset at all (SHADPS4_VR=0, SHADPS4_OPENXR=0)
#   xr      = real OpenXR session (SHADPS4_VR=1, SHADPS4_OPENXR=1): only with the headset free
param([ValidateSet('virtual','flat','xr')][string]$Mode = 'virtual', [int]$Seconds = 90, [switch]$Deploy,
      [string]$Tag = '', [hashtable]$Env = @{}, [string]$Build = 'C:\Games\Farpoint-src\build\win-us\shadps4.exe',
      [string]$Script = '', [string]$Shots = '', [switch]$SkipVideo)
$ErrorActionPreference = 'Continue'
$root = 'C:\Games\SuperStardustUltraVR'; $pcvr = Join-Path $root 'pc-vr'; $exe = Join-Path $pcvr 'shadps4.exe'
$eboot = Join-Path $root 'games\CUSA05951\eboot.bin'
$log = Join-Path $pcvr 'user\log\shad_log.txt'
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$out = Join-Path 'C:\Games\ssuvr-work\runs' ("$stamp-$Mode" + $(if ($Tag) { "-$Tag" } else { '' }))
New-Item -ItemType Directory -Force $out | Out-Null
if (Get-Process shadps4 -ErrorAction SilentlyContinue) { throw 'shadps4.exe is already running; not starting another.' }
if ($Deploy) { Copy-Item -LiteralPath $Build -Destination $exe -Force; "deployed $Build -> $exe" }
if (-not (Test-Path -LiteralPath $exe)) { throw "no emulator at $exe (use -Deploy)" }
$userDir = Join-Path $pcvr 'user'; New-Item -ItemType Directory -Force (Join-Path $userDir 'log') | Out-Null
$cfg = Join-Path $userDir 'config.json'
if (-not (Test-Path -LiteralPath $cfg)) {
    $src = Join-Path $pcvr 'config-default.json'
    if (-not (Test-Path -LiteralPath $src)) { $src = 'C:\Games\Farpoint\pc-vr\config-default.json' }
    $json = Get-Content -LiteralPath $src -Raw | ConvertFrom-Json
    $json.Input.use_special_pad = $false; $json.Input.special_pad_class = 0
    $json | ConvertTo-Json -Depth 30 | Set-Content -LiteralPath $cfg -Encoding UTF8
    "wrote $cfg from $src"
}
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System; using System.Runtime.InteropServices;
public static class W { [StructLayout(LayoutKind.Sequential)] public struct R { public int L, T, Rr, B; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out R r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags); }
'@
# Captures the window's own content (PrintWindow with PW_RENDERFULLCONTENT), even when another window covers it.
function Shot([System.Diagnostics.Process]$p, [string]$file) {
    $p.Refresh(); $h = $p.MainWindowHandle
    if ($h -eq [IntPtr]::Zero) { return $false }
    $r = New-Object W+R; [W]::GetWindowRect($h, [ref]$r) | Out-Null
    $w = $r.Rr - $r.L; $hh = $r.B - $r.T
    if ($w -le 50 -or $hh -le 50) { return $false }
    $bmp = New-Object System.Drawing.Bitmap $w, $hh
    $gr = [System.Drawing.Graphics]::FromImage($bmp); $hdc = $gr.GetHdc()
    $ok = [W]::PrintWindow($h, $hdc, 2); $gr.ReleaseHdc($hdc)
    if (-not $ok) { $gr.CopyFromScreen($r.L, $r.T, 0, 0, $bmp.Size) }
    $bmp.Save($file, [System.Drawing.Imaging.ImageFormat]::Png); $gr.Dispose(); $bmp.Dispose()
    return $true
}
# Environment: the launcher's headset settings, minus the Farpoint-only ones.
$e = @{ SHADPS4_XR_GAMEPAD_LAYOUT='1'; SHADPS4_XR_PAD_HAND='right'; SHADPS4_XR_WAIT='0'; SHADPS4_XR_PAUSE='0';
        SHADPS4_VR_FOV='100'; SHADPS4_VR_WINDOW_FPS='60'; SHADPS4_VR_PACE='1'; SHADPS4_GPU_WAIT_TIMING='1';
        SHADPS4_SUBMIT_BATCH_SIZE='32'; SHADPS4_TITLE_RESOLUTION='title'; SHADPS4_TITLE_EYE_WIDTH='' }
switch ($Mode) {
    'virtual' { $e.SHADPS4_VR = '1'; $e.SHADPS4_OPENXR = '0' }
    'flat'    { $e.SHADPS4_VR = '0'; $e.SHADPS4_OPENXR = '0' }
    'xr'      { $e.SHADPS4_VR = '1'; $e.SHADPS4_OPENXR = '1' }
}
foreach ($k in @('SHADPS4_XR_SWAP_CIRCLE_R3','SHADPS4_XR_SWAP_GRIPS','SHADPS4_XR_TOGGLE_L3','SHADPS4_XR_PAD_PITCH','SHADPS4_XR_PAD_OFFSET',
                 'SHADPS4_FARPOINT_OFFLINE_SCORE','SHADPS4_FARPOINT_EYE_WIDTH','SHADPS4_INPUT_SCRIPT','SHADPS4_LIVE_INPUT',
                 'SHADPS4_XR_TEST_PRESS','SHADPS4_XR_TEST_TURN','SHADPS4_VR_FPS_CAP')) { $e[$k] = '' }
if ($Mode -eq 'xr') { $e['SHADPS4_VR_FOV_OF'] = 'headset' }   # as the launcher: the game draws the headset's own field of view
foreach ($k in $Env.Keys) { $e[$k] = [string]$Env[$k] }
# An input script (see src/input/scripted_input.h for the format) plays the game on the first controller.
if ($Script) { $e['SHADPS4_INPUT_SCRIPT'] = (Resolve-Path -LiteralPath $Script).Path }
if ($SkipVideo) { $e['SHADPS4_AVPLAYER_SKIP'] = '1' }
# Remember this shell's own values: the run's settings are given back after the start (see below).
$prevEnv = @{}; foreach ($k in $e.Keys) { $prevEnv[$k] = [Environment]::GetEnvironmentVariable($k, 'Process') }
foreach ($k in $e.Keys) { if ($e[$k] -eq '') { [Environment]::SetEnvironmentVariable($k, $null, 'Process') } else { [Environment]::SetEnvironmentVariable($k, $e[$k], 'Process') } }
($e.GetEnumerator() | Sort-Object Name | ForEach-Object { "$($_.Name)=$($_.Value)" }) -join "`n" | Set-Content (Join-Path $out 'env.txt')
if (Test-Path -LiteralPath $log) { Move-Item -LiteralPath $log -Destination (Join-Path $out 'previous_shad_log.txt') -Force }
$t0 = Get-Date
$p = Start-Process -FilePath $exe -ArgumentList @('-g', "`"$eboot`"") -WorkingDirectory $pcvr -PassThru `
        -RedirectStandardOutput (Join-Path $out 'stdout.txt') -RedirectStandardError (Join-Path $out 'stderr.txt')
# The emulator has its environment now. Give this shell back the one it had, so a later run in the same
# session (rate-test.ps1 runs several) does not inherit this run's settings (it did, 2026-11-07: a sweep leaked into fps runs).
foreach ($k in $prevEnv.Keys) { [Environment]::SetEnvironmentVariable($k, $prevEnv[$k], 'Process') }
"started pid $($p.Id) mode=$Mode for ${Seconds}s -> $out"
$taken = @(); $marks = if ($Shots) { @($Shots -split ',' | ForEach-Object { [int]$_.Trim() }) | Sort-Object -Unique } else { @(30, 60, [Math]::Max(10, $Seconds - 5)) | Sort-Object -Unique }
while (-not $p.HasExited -and ((Get-Date) - $t0).TotalSeconds -lt $Seconds) {
    Start-Sleep -Seconds 2
    $el = [int]((Get-Date) - $t0).TotalSeconds
    foreach ($m in $marks) { if ($el -ge $m -and $taken -notcontains $m) { $taken += $m; if (Shot $p (Join-Path $out "shot-$m.png")) { "shot at ${m}s" } } }
}
$alive = -not $p.HasExited
$code = if ($alive) { 'running' } else { "exit $($p.ExitCode)" }
if ($alive) { Stop-Process -Id $p.Id -Force; Start-Sleep 2 }
$secs = [int]((Get-Date) - $t0).TotalSeconds
if (Test-Path -LiteralPath $log) { Copy-Item -LiteralPath $log -Destination (Join-Path $out 'shad_log.txt') -Force }
$lines = if (Test-Path -LiteralPath $log) { Get-Content -LiteralPath $log } else { @() }
$crit = @($lines | Where-Object { $_ -match '<Critical>|Unreachable|assert|Unhandled|Exception' })
$err = @($lines | Where-Object { $_ -match '<Error>' })
$warn = @($lines | Where-Object { $_ -match '<Warning>' })
$frames = ($lines | Select-String -Pattern 'presentId=(\d+)' -AllMatches | ForEach-Object { $_.Matches } | ForEach-Object { [int]$_.Groups[1].Value } | Measure-Object -Maximum).Maximum
$sum = @()
$sum += "RESULT: $code after ${secs}s | frames $([int]$frames) | crit $($crit.Count) err $($err.Count) warn $($warn.Count) | mode $Mode | $out"
$sum += '--- game stdout/stderr ---'
$sum += ($lines | Where-Object { $_ -match '\[stdout\]|\[stderr\]' } | ForEach-Object { $_ -replace '^.*log_flush: ', '' } | Select-Object -First 60)
$sum += '--- Core.Vr / Hmd / VrTracker / Camera / stereo (first 120 distinct) ---'
$sum += ($lines | Where-Object { $_ -match '\[Core\.Vr\]|\[Lib\.Hmd\]|\[Lib\.VrTracker\]|\[Lib\.Camera\]|stereo|Stereo|HmdSetupDialog' } | ForEach-Object { $_ -replace '^\[[^\]]*\] ', '' } | Select-Object -Unique -First 120)
$sum += '--- Critical ---'; $sum += ($crit | Select-Object -First 20)
$sum += '--- Error (distinct, first 40) ---'; $sum += ($err | ForEach-Object { $_ -replace '^\[[^\]]*\] ', '' } | Select-Object -Unique -First 40)
$sum += '--- Warning (distinct, first 40) ---'; $sum += ($warn | ForEach-Object { $_ -replace '^\[[^\]]*\] ', '' } | Select-Object -Unique -First 40)
$sum += '--- last 15 lines ---'; $sum += ($lines | Select-Object -Last 15)
$sum | Set-Content (Join-Path $out 'summary.txt')
$sum | Select-Object -First 220
