# Test helper: puts Super Stardust Ultra VR's render size back to the game's own (scene 1920x1080, eyes 960x1080)
# for a smoke.ps1 run, and restores the launcher's files afterwards. The launcher rewrites all of these on every
# start anyway, so a user launch always gets the saved RenderPercent back.
# usage: scene-native.ps1 [-Restore]
param([switch]$Restore)
$ErrorActionPreference = 'Stop'
$root = 'C:\Games\SuperStardustUltraVR'
$files = @(
    (Join-Path $root 'games\CUSA05951\Plan18.HMDconfig'),
    (Join-Path $root 'pc-vr\user\patches\ssuvr-res\CUSA05951.xml'),
    (Join-Path $root 'pc-vr\user\patches\ssuvr-heap\CUSA05951.xml')
)
$backup = 'C:\Games\ssuvr-work\scene-backup'
if ($Restore) {
    foreach ($f in $files) {
        $b = Join-Path $backup ((Split-Path (Split-Path $f -Parent) -Leaf) + '__' + (Split-Path $f -Leaf))
        Copy-Item -LiteralPath $b -Destination $f -Force
        "restored $f"
    }
    return
}
if (Get-Process shadps4 -ErrorAction SilentlyContinue) { throw 'shadps4.exe is running; not touching its files.' }
New-Item -ItemType Directory -Force $backup | Out-Null
foreach ($f in $files) {
    $b = Join-Path $backup ((Split-Path (Split-Path $f -Parent) -Leaf) + '__' + (Split-Path $f -Leaf))
    Copy-Item -LiteralPath $f -Destination $b -Force
}
Copy-Item -LiteralPath (Join-Path $root 'pc-vr\Plan18.HMDconfig.original') -Destination $files[0] -Force
foreach ($f in $files[1..2]) {
    $text = [IO.File]::ReadAllText($f) -replace 'isEnabled="true"', 'isEnabled="false"'
    [IO.File]::WriteAllText($f, $text, [Text.UTF8Encoding]::new($false))
}
'scene back to the game''s own size (1920x1080, eyes 960x1080); backups in ' + $backup
