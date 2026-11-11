# Super Stardust Ultra VR (CUSA05951, EU, patch 1.03) on PC VR: starts the shadPS4 fork built from
# C:\Games\Farpoint-src (branch ssuvr) with the environment this title needs. Derived from the
# Farpoint v0.9.6 launcher (farpoint-launcher/launch-farpoint.ps1), minus Farpoint's pkg
# extraction, pak/resolution profiles and Aim-controller knobs: this is a DualShock-only title.
# Started by "Play Super Stardust Ultra VR.bat" (settings window) or
# "Play Super Stardust Ultra VR (Steam).bat" (-NoGui: saved settings, for Steam / SteamVR).
#
# Smoke tests (no headset needed):
#   -NoGui -Headset off -Seconds 120              flat on the monitor (SHADPS4_OPENXR=0, SHADPS4_VR=0)
#   -NoGui -Headset on -NoOpenXr -Seconds 120     virtual headset on the monitor (OpenXR off, SHADPS4_VR=1)
#   -ValidateOnly                                 checks the game files and prints the environment table
# -Fps 120|144 draws that many frames a second; -Fps headset follows the headset's refresh rate at start
# (120 or 144 Hz on a Steam Frame). -RateMode world (default) runs the game world at that rate too, with
# its per-frame values (lerps, dampings, frame counts, particle step) rescaled to play as at 60
# (SHADPS4_SSU_FPS). -RateMode draw keeps the world at its own 59.94 updates a second and only draws
# the extra frames with a fresh head pose (SHADPS4_SSU_DRAW_FPS): play exactly as at 60, but the world
# and the camera move every other frame.
# Given on the command line with -NoGui it applies to that run only
# ("Play Super Stardust Ultra VR at 120 fps.bat"); the settings window saves it.
param(
    [string]$GamePath,
    [ValidateSet(0,30,60)][int]$MirrorFps = 30,
    # Percent of the headset's own field of view the game draws (100 = the whole view; a PSVR's is ~100 of 110 deg).
    [ValidateRange(60,120)][int]$FovPercent = 100,
    # Eye render target size in percent of the PSVR's 960x1080 an eye (200 = 1920x2160), through the memory
    # patch the launcher writes to user\patches\ssuvr-res\. 100 leaves the game at its original size.
    [ValidateSet(100,150,200,250,300,400)][int]$RenderPercent = 200,
    # The game's own frame rate: 60 (original, 59.94 updates a second), 120, 144, or headset (its refresh rate).
    [ValidateSet('60','120','144','headset')][string]$Fps = '60',
    # Above 60: world = the game world updates at that rate (rescaled to play as at 60); draw = it keeps
    # 59.94 updates and the extra frames only get a fresh head pose.
    [ValidateSet('world','draw')][string]$RateMode = 'world',
    # How big the world feels in the headset, in percent: 100 as the game has it (1 game unit = 1 cm). The
    # game is given the eye separation and head movement times 100/WorldSize (SHADPS4_VR_WORLD_SIZE).
    [ValidateSet(50,60,70,80,90,100)][int]$WorldSize = 100,
    [ValidateSet('on','off','auto')][string]$Headset = 'on',
    # on: the memory patch in user\patches\ssuvr\ boots the game straight to the title screen; the
    # intro video is also ended at once (SHADPS4_AVPLAYER_SKIP) and, if pc-vr\skip-intro.txt holds any
    # presses, they are replayed while the controllers keep working.
    [ValidateSet('on','off')][string]$SkipIntro = 'on',
    [ValidateRange(0,86400)][int]$Seconds = 0,
    [switch]$NoGui,
    [switch]$ValidateOnly,
    [switch]$NoOpenXr,
    [object]$Env
)
$ErrorActionPreference = 'Stop'
$installRoot = Split-Path $PSScriptRoot -Parent
$exe = Join-Path $PSScriptRoot 'shadps4.exe'
$settingsFile = Join-Path $PSScriptRoot 'ssuvr-settings.json'
$configDefault = Join-Path $PSScriptRoot 'config-default.json'
$userDir = Join-Path $PSScriptRoot 'user'
$configPath = Join-Path $userDir 'config.json'
$title = 'Super Stardust Ultra VR'
$titleId = 'CUSA05951'
# eboot.bin SHA256 from the user's own dump: patch 1.03 is the supported executable; base 1.00 is allowed with a warning.
$hashPatch103 = '4A6349CB36EF0BF1F22288523222C8FD580E321A373E2233D80A03B5ECEBCA5D'
$hashBase100 = '9BB963806CF459AAF9D9FCFF6B8AFBB0D110691D37B3A4B24D28A6CFAF3A13DA'
$headsetChoices = @('on', 'auto', 'off')
# Eye render target sizes: percent of 960x1080, widths rounded up to 16 and heights to 8 pixels.
$renderChoices = @(100, 150, 200, 250, 300, 400)
function Get-EyeSize([int]$percent) {
    $w = [int][Math]::Ceiling(960.0 * $percent / 100 / 16) * 16
    $h = [int][Math]::Ceiling(1080.0 * $percent / 100 / 8) * 8
    $note = if ($percent -eq 100) { ' (original)' } elseif ($percent -eq 200) { ' (recommended)' } elseif ($percent -eq 400) { ' (4K an eye)' } else { '' }
    @{ Width = $w; Height = $h; Text = "$percent% - ${w}x${h} an eye$note" }
}
$headsetTexts = @('Headset on - OpenXR headset, virtual PSVR connected',
                  'Auto - headset when the OpenXR runtime has one, else the window',
                  'Off - flat on the monitor, no VR')

# SHA-256 of a file as upper-case hex, through .NET so it works even when Windows PowerShell inherits a
# PowerShell 7 PSModulePath (then Get-FileHash cannot be found).
function Get-Sha256Hex([string]$path) {
    $sha = [System.Security.Cryptography.SHA256]::Create(); $fs = [IO.File]::OpenRead($path)
    try { ([BitConverter]::ToString($sha.ComputeHash($fs))).Replace('-', '') } finally { $fs.Dispose(); $sha.Dispose() }
}
function Write-Utf8([string]$path, [string]$text) {
    # UTF-8 without BOM: the emulator and other tools read these files too.
    [IO.File]::WriteAllText($path, $text, (New-Object System.Text.UTF8Encoding($false)))
}

# What a param.sfo says, by name (TITLE_ID, APP_VER, ...). Layout: "\0PSF" magic, u32 version,
# u32 key table offset, u32 data table offset, u32 entry count, then 16-byte index entries of
# u16 key offset, u16 format (0x0204 utf8 text, 0x0404 u32), u32 length, u32 max length, u32 data offset.
function Read-Sfo([string]$path) {
    $values = @{}
    $bytes = [IO.File]::ReadAllBytes($path)
    if ($bytes.Length -lt 20 -or [BitConverter]::ToUInt32($bytes, 0) -ne 0x46535000) { throw "Not a param.sfo: $path" }
    $keys = [BitConverter]::ToInt32($bytes, 8)
    $data = [BitConverter]::ToInt32($bytes, 12)
    $count = [BitConverter]::ToInt32($bytes, 16)
    for ($i = 0; $i -lt $count; $i++) {
        $at = 20 + 16 * $i
        $keyAt = $keys + [BitConverter]::ToUInt16($bytes, $at)
        $format = [BitConverter]::ToUInt16($bytes, $at + 2)
        $length = [BitConverter]::ToInt32($bytes, $at + 4)
        $valueAt = $data + [BitConverter]::ToInt32($bytes, $at + 12)
        $end = [Array]::IndexOf($bytes, [byte]0, $keyAt)
        if ($end -lt $keyAt) { continue }
        $key = [Text.Encoding]::ASCII.GetString($bytes, $keyAt, $end - $keyAt)
        if ($format -eq 0x0204 -and $length -gt 0) {
            $values[$key] = [Text.Encoding]::UTF8.GetString($bytes, $valueAt, $length - 1).TrimEnd([char]0)
        } elseif ($format -eq 0x0404 -and $length -ge 4) {
            $values[$key] = [BitConverter]::ToUInt32($bytes, $valueAt)
        }
    }
    return $values
}

function Set-ConfigValue($object, [string]$section, [string]$name, $value) {
    if (-not $object.PSObject.Properties[$section] -or $null -eq $object.$section) {
        $object | Add-Member -NotePropertyName $section -NotePropertyValue ([pscustomobject]@{}) -Force
    }
    $object.$section | Add-Member -NotePropertyName $name -NotePropertyValue $value -Force
}

# Defaults, then the saved choices for whatever was not given on the command line.
if (-not $GamePath) { $GamePath = Join-Path $installRoot "games\$titleId\eboot.bin" }
$fpsChoices = @('60','120','144','headset')
$worldChoices = @(100, 90, 80, 70, 60, 50)
$savedFps = '60'
$savedRateMode = 'world'
if (Test-Path -LiteralPath $settingsFile) {
    $saved = $null
    try { $saved = Get-Content -LiteralPath $settingsFile -Raw | ConvertFrom-Json } catch { Write-Warning "Ignoring unreadable $settingsFile" }
    if ($saved) {
        if (-not $PSBoundParameters.ContainsKey('GamePath') -and $saved.GamePath) { $GamePath = [string]$saved.GamePath }
        if (-not $PSBoundParameters.ContainsKey('MirrorFps') -and $saved.MirrorFps -in @(0,30,60)) { $MirrorFps = [int]$saved.MirrorFps }
        if (-not $PSBoundParameters.ContainsKey('FovPercent') -and $saved.FovPercent -ge 60 -and $saved.FovPercent -le 120) { $FovPercent = [int]$saved.FovPercent }
        if (-not $PSBoundParameters.ContainsKey('RenderPercent') -and $saved.RenderPercent -in $renderChoices) { $RenderPercent = [int]$saved.RenderPercent }
        if (-not $PSBoundParameters.ContainsKey('Headset') -and $saved.Headset -in $headsetChoices) { $Headset = [string]$saved.Headset }
        if (-not $PSBoundParameters.ContainsKey('SkipIntro') -and $saved.SkipIntro -in @('on','off')) { $SkipIntro = [string]$saved.SkipIntro }
        if (-not $PSBoundParameters.ContainsKey('WorldSize') -and $saved.WorldSize -in $worldChoices) { $WorldSize = [int]$saved.WorldSize }
        if ([string]$saved.Fps -in $fpsChoices) { $savedFps = [string]$saved.Fps }
        if ($saved.RateMode -in @('world','draw')) { $savedRateMode = [string]$saved.RateMode }
    }
}
if (-not $PSBoundParameters.ContainsKey('Fps')) { $Fps = $savedFps }
if (-not $PSBoundParameters.ContainsKey('RateMode')) { $RateMode = $savedRateMode }
$skipScript = Join-Path $PSScriptRoot 'skip-intro.txt'

if (-not $NoGui -and -not $ValidateOnly) {
    Add-Type -AssemblyName System.Windows.Forms
    Add-Type -AssemblyName System.Drawing
    [System.Windows.Forms.Application]::EnableVisualStyles()
    $form = New-Object System.Windows.Forms.Form
    $form.Text = 'Super Stardust Ultra VR - PC VR (prototype)'
    $form.ClientSize = New-Object System.Drawing.Size(620,519)
    $form.StartPosition = 'CenterScreen'
    $form.FormBorderStyle = 'FixedDialog'
    $form.MaximizeBox = $false
    $label = New-Object System.Windows.Forms.Label
    $label.Text = 'Super Stardust Ultra VR 1.03 (CUSA05951 EU), DualShock controls. Prototype: the headset path is untested.'
    $label.SetBounds(18,16,585,38); $form.Controls.Add($label)
    $pathLabel = New-Object System.Windows.Forms.Label
    $pathLabel.Text = 'Game executable (games\CUSA05951\eboot.bin, merged base 1.00 + patch 1.03)'; $pathLabel.SetBounds(18,60,560,20); $form.Controls.Add($pathLabel)
    $pathBox = New-Object System.Windows.Forms.TextBox
    $pathBox.Text = $GamePath; $pathBox.SetBounds(18,83,490,24); $form.Controls.Add($pathBox)
    $browse = New-Object System.Windows.Forms.Button
    $browse.Text = 'Browse...'; $browse.SetBounds(518,81,83,27); $form.Controls.Add($browse)
    $browse.Add_Click({
        $dialog = New-Object System.Windows.Forms.OpenFileDialog
        $dialog.Filter = 'Game executable (eboot.bin)|eboot.bin'
        $gamesFolder = Join-Path $installRoot 'games'
        if (Test-Path -LiteralPath $gamesFolder) { $dialog.InitialDirectory = $gamesFolder }
        if ($dialog.ShowDialog() -eq 'OK') { $pathBox.Text = $dialog.FileName }
        $dialog.Dispose()
    })
    $headsetLabel = New-Object System.Windows.Forms.Label
    $headsetLabel.Text = 'Headset'; $headsetLabel.SetBounds(18,123,360,20); $form.Controls.Add($headsetLabel)
    $headsetCombo = New-Object System.Windows.Forms.ComboBox
    $headsetCombo.DropDownStyle = 'DropDownList'; $headsetCombo.SetBounds(18,147,360,25)
    foreach ($text in $headsetTexts) { [void]$headsetCombo.Items.Add($text) }
    $headsetCombo.SelectedIndex = [array]::IndexOf($headsetChoices, $Headset); $form.Controls.Add($headsetCombo)
    $mirrorLabel = New-Object System.Windows.Forms.Label
    $mirrorLabel.Text = 'Desktop mirror'; $mirrorLabel.SetBounds(395,123,205,20); $form.Controls.Add($mirrorLabel)
    $mirror = New-Object System.Windows.Forms.ComboBox
    $mirror.DropDownStyle = 'DropDownList'; $mirror.SetBounds(395,147,205,25)
    [void]$mirror.Items.Add('60 FPS'); [void]$mirror.Items.Add('30 FPS'); [void]$mirror.Items.Add('Uncapped')
    $mirror.SelectedIndex = if ($MirrorFps -eq 60) { 0 } elseif ($MirrorFps -eq 30) { 1 } else { 2 }; $form.Controls.Add($mirror)
    $renderLabel = New-Object System.Windows.Forms.Label
    $renderLabel.Text = 'Render resolution (higher is sharper, costs GPU time)'; $renderLabel.SetBounds(18,180,360,20); $form.Controls.Add($renderLabel)
    $renderCombo = New-Object System.Windows.Forms.ComboBox
    $renderCombo.DropDownStyle = 'DropDownList'; $renderCombo.SetBounds(18,204,360,25)
    foreach ($choice in $renderChoices) { [void]$renderCombo.Items.Add((Get-EyeSize $choice).Text) }
    $renderCombo.SelectedIndex = [Array]::IndexOf($renderChoices, $RenderPercent); $form.Controls.Add($renderCombo)
    $fpsLabel = New-Object System.Windows.Forms.Label
    $fpsLabel.Text = 'Game frame rate'; $fpsLabel.SetBounds(395,180,205,20); $form.Controls.Add($fpsLabel)
    $fpsCombo = New-Object System.Windows.Forms.ComboBox
    $fpsCombo.DropDownStyle = 'DropDownList'; $fpsCombo.SetBounds(395,204,205,25)
    foreach ($label in @('60 FPS (original)', '120 FPS', '144 FPS', "Match the headset's refresh")) { [void]$fpsCombo.Items.Add($label) }
    $fpsCombo.SelectedIndex = [Math]::Max(0, [Array]::IndexOf($fpsChoices, [string]$Fps)); $form.Controls.Add($fpsCombo)
    $fovLabel = New-Object System.Windows.Forms.Label
    $fovLabel.Text = "Rendered FOV: $FovPercent% of the headset's view"; $fovLabel.SetBounds(18,239,580,20); $form.Controls.Add($fovLabel)
    $fovSlider = New-Object System.Windows.Forms.TrackBar
    $fovSlider.Minimum = 60; $fovSlider.Maximum = 120
    $fovSlider.TickFrequency = 10; $fovSlider.SmallChange = 1; $fovSlider.LargeChange = 10
    $fovSlider.Value = $FovPercent; $fovSlider.SetBounds(18,262,582,45)
    $fovSlider.Add_ValueChanged({ $fovLabel.Text = "Rendered FOV: $($fovSlider.Value)% of the headset's view" })
    $form.Controls.Add($fovSlider)
    $fovHelp = New-Object System.Windows.Forms.Label
    $fovHelp.Text = '60% (narrower, sharper)          100% (the whole headset view)          120% (wider)'
    $fovHelp.SetBounds(18,309,580,20); $form.Controls.Add($fovHelp)
    $worldLabel = New-Object System.Windows.Forms.Label
    $worldLabel.Text = "World size (smaller: the play sphere looks smaller and nearer; 100 = as the game has it)"; $worldLabel.SetBounds(18,336,580,20); $form.Controls.Add($worldLabel)
    $worldCombo = New-Object System.Windows.Forms.ComboBox
    $worldCombo.DropDownStyle = 'DropDownList'; $worldCombo.SetBounds(18,360,360,25)
    foreach ($choice in $worldChoices) { [void]$worldCombo.Items.Add($(if ($choice -eq 100) { '100% (as the game draws it)' } else { "$choice%" })) }
    $worldCombo.SelectedIndex = [Math]::Max(0, [Array]::IndexOf($worldChoices, $WorldSize)); $form.Controls.Add($worldCombo)
    $skipBox = New-Object System.Windows.Forms.CheckBox
    $skipBox.Text = 'Skip the intro: boot straight to the title screen (memory patch; logos, legal text and calibrate prompt skipped)'
    $skipBox.Checked = ($SkipIntro -eq 'on'); $skipBox.SetBounds(18,393,582,22); $form.Controls.Add($skipBox)
    $worldRateBox = New-Object System.Windows.Forms.CheckBox
    $worldRateBox.Text = 'Above 60 FPS, run the game world at that rate too (smoother; unticked: world at 60, only the head pose per frame)'
    $worldRateBox.Checked = ($RateMode -eq 'world'); $worldRateBox.SetBounds(18,419,582,22); $form.Controls.Add($worldRateBox)
    $info = New-Object System.Windows.Forms.Label
    $info.Text = 'Headset on: connect the headset through your active OpenXR runtime before starting. Off plays flat on the monitor with a gamepad. The mirror setting does not cap headset frames. Logs: pc-vr\user\log.'
    $info.SetBounds(18,443,580,40); $form.Controls.Add($info)
    $play = New-Object System.Windows.Forms.Button
    $play.Text = 'Start'; $play.SetBounds(455,480,145,32)
    $play.DialogResult = 'OK'; $form.AcceptButton = $play; $form.Controls.Add($play)
    if ($form.ShowDialog() -ne 'OK') { $form.Dispose(); return }
    $GamePath = $pathBox.Text
    $Headset = $headsetChoices[$headsetCombo.SelectedIndex]
    $MirrorFps = @(60,30,0)[$mirror.SelectedIndex]
    $FovPercent = $fovSlider.Value
    $RenderPercent = $renderChoices[$renderCombo.SelectedIndex]
    $Fps = $fpsChoices[$fpsCombo.SelectedIndex]
    $WorldSize = $worldChoices[$worldCombo.SelectedIndex]
    $SkipIntro = if ($skipBox.Checked) { 'on' } else { 'off' }
    $RateMode = if ($worldRateBox.Checked) { 'world' } else { 'draw' }
    $form.Dispose()
}
# A frame rate given on the command line without the window is for this run only; the saved one stays.
$fpsToSave = if ($NoGui -and $PSBoundParameters.ContainsKey('Fps')) { $savedFps } else { $Fps }
$rateModeToSave = if ($NoGui -and $PSBoundParameters.ContainsKey('RateMode')) { $savedRateMode } else { $RateMode }

# The game files: the right executable (by hash) with its own metadata beside it.
if (-not (Test-Path -LiteralPath $GamePath -PathType Leaf)) { throw "Game executable not found: $GamePath. Expected <install>\games\$titleId\eboot.bin (merged base 1.00 + patch 1.03 from your own dump)." }
$GamePath = (Resolve-Path -LiteralPath $GamePath).Path
if ([IO.Path]::GetFileName($GamePath) -ne 'eboot.bin') { throw "Select the $title eboot.bin." }
$gameHash = (Get-Sha256Hex $GamePath)
$gameVersion = if ($gameHash -eq $hashPatch103) { '1.03' } elseif ($gameHash -eq $hashBase100) { '1.00' } else { $null }
if (-not $gameVersion) { throw "Unknown eboot.bin (SHA256 $gameHash). This build supports $title $titleId patch 1.03 ($hashPatch103); base 1.00 ($hashBase100) is allowed but untested." }
if ($gameVersion -eq '1.00') { Write-Warning "This is the base 1.00 executable. Patch 1.03 is the supported one; 1.00 is allowed but untested." }
$sfoPath = Join-Path (Join-Path (Split-Path $GamePath -Parent) 'sce_sys') 'param.sfo'
if (-not (Test-Path -LiteralPath $sfoPath -PathType Leaf)) { throw "Missing $sfoPath beside eboot.bin. Copy the complete sce_sys folder from the same dump." }
$sfo = Read-Sfo $sfoPath
if ([string]$sfo['TITLE_ID'] -ne $titleId) { throw "sce_sys\param.sfo names title '$($sfo['TITLE_ID'])', not $titleId. Do not mix metadata from another game or region." }
$appVersion = [string]$sfo['APP_VER']

# The environment the emulator starts with. An empty value means: removed from the environment.
$knobs = [ordered]@{}
switch ($Headset) {
    'on'   { $knobs['SHADPS4_OPENXR'] = '1'; $knobs['SHADPS4_VR'] = '1' }
    'off'  { $knobs['SHADPS4_OPENXR'] = '0'; $knobs['SHADPS4_VR'] = '0' }
    'auto' { $knobs['SHADPS4_OPENXR'] = '1'; $knobs['SHADPS4_VR'] = '' }
}
# -NoOpenXr: no OpenXR runtime is looked for, but the virtual headset still follows -Headset
# (SHADPS4_VR=1 gives the desktop stereo view with demo motion: the monitor smoke test).
if ($NoOpenXr) { $knobs['SHADPS4_OPENXR'] = '0' }
# The title is told the headset's own field of view (a PSVR's 100 degrees would leave a border in a
# 110-degree headset); the game draws with whatever it is told. -FovPercent scales it; 100 is the whole view.
$knobs['SHADPS4_VR_FOV'] = [string]$FovPercent
$knobs['SHADPS4_VR_FOV_OF'] = 'headset'
# Desktop mirror cap; it does not cap headset frames.
$knobs['SHADPS4_VR_WINDOW_FPS'] = [string]$MirrorFps
# One host refresh per two guest vblanks (the title runs at the headset rate).
$knobs['SHADPS4_VR_PACE'] = '1'
# Frames a second; empty = the original 59.94. world: SHADPS4_SSU_FPS runs the game world at that rate
# and rescales its per-update values (148 registered lerps/dampings/frame counts, 11 lerps/dampings in
# the code, the particle step; 2026-11-07: shot reach, ship speed and boost match 60). draw:
# SHADPS4_SSU_DRAW_FPS keeps 59.94 world updates and gives the frames between a fresh head pose.
$knobs['SHADPS4_SSU_FPS'] = if ($Fps -ne '60' -and $RateMode -eq 'world') { [string]$Fps } else { '' }
$knobs['SHADPS4_SSU_DRAW_FPS'] = if ($Fps -ne '60' -and $RateMode -eq 'draw') { [string]$Fps } else { '' }
# World size: the game takes the eye separation (sceHmdGet2DEyeOffset / tracker eye poses) and the head
# position in metres and works in 100 units a metre; HMD.config's STEREO_SEPARATION does not set the eyes.
# SHADPS4_VR_WORLD_SIZE gives it both times 100/percent: measured 2026-11-07 (ssuvr-work\head-test.ps1),
# 100% -> eyes 6.30 units apart, 100 units per metre of head movement; 50% -> 12.60 and 200.
$knobs['SHADPS4_VR_WORLD_SIZE'] = if ($WorldSize -eq 100) { '' } else { [string]$WorldSize }
# Do not wait for a headset at start, do not pause the game when the headset is taken off.
$knobs['SHADPS4_XR_WAIT'] = '0'
$knobs['SHADPS4_XR_PAUSE'] = '0'
# VR controllers act as one gamepad (DualShock layout); the virtual pad sits in the right hand.
$knobs['SHADPS4_XR_GAMEPAD_LAYOUT'] = '1'
$knobs['SHADPS4_XR_PAD_HAND'] = 'right'
$knobs['SHADPS4_GPU_WAIT_TIMING'] = '1'
$inheritedBatch = [Environment]::GetEnvironmentVariable('SHADPS4_SUBMIT_BATCH_SIZE', 'Process')
$knobs['SHADPS4_SUBMIT_BATCH_SIZE'] = if ($inheritedBatch) { $inheritedBatch } else { '32' }
# Farpoint-only controller/offline knobs, input scripting and the Astro-specific resolution
# patches must not leak into this title (as the Farpoint launcher does for Astro's).
foreach ($name in @('SHADPS4_XR_SWAP_CIRCLE_R3', 'SHADPS4_XR_SWAP_GRIPS', 'SHADPS4_XR_TOGGLE_L3', 'SHADPS4_XR_PAD_PITCH',
                    'SHADPS4_XR_PAD_OFFSET', 'SHADPS4_FARPOINT_OFFLINE_SCORE', 'SHADPS4_FARPOINT_EYE_WIDTH', 'SHADPS4_INPUT_SCRIPT',
                    'SHADPS4_LIVE_INPUT', 'SHADPS4_XR_TEST_PRESS', 'SHADPS4_XR_TEST_TURN', 'SHADPS4_TITLE_EYE_WIDTH', 'SHADPS4_VR_FPS_CAP')) {
    $knobs[$name] = ''
}
$knobs['SHADPS4_TITLE_RESOLUTION'] = 'title'
# Skipping the intro: the patch in user\patches\ssuvr\ does the work; videos end as soon as they start
# as a fallback, and the input script is replayed only when it holds at least one press (the headset's
# controllers stay in use beside it, SHADPS4_XR_CONTROLLERS).
if ($SkipIntro -eq 'on') {
    $knobs['SHADPS4_AVPLAYER_SKIP'] = '1'
    $hasPresses = (Test-Path -LiteralPath $skipScript -PathType Leaf) -and
        [bool](Get-Content -LiteralPath $skipScript | Where-Object { $_.Trim() -ne '' -and -not $_.TrimStart().StartsWith('#') })
    if ($hasPresses) {
        $knobs['SHADPS4_INPUT_SCRIPT'] = $skipScript
        $knobs['SHADPS4_XR_CONTROLLERS'] = '1'
    } else {
        $knobs['SHADPS4_XR_CONTROLLERS'] = ''
    }
} else {
    $knobs['SHADPS4_AVPLAYER_SKIP'] = ''
    $knobs['SHADPS4_XR_CONTROLLERS'] = ''
}
# Extra NAME=value pairs from -Env (a hashtable, or strings 'NAME=value') win over everything above.
if ($null -ne $Env) {
    if ($Env -is [System.Collections.IDictionary]) {
        foreach ($key in @($Env.Keys)) { $knobs[[string]$key] = [string]$Env[$key] }
    } else {
        foreach ($item in @($Env)) {
            $text = [string]$item
            $at = $text.IndexOf('=')
            if ($at -lt 1) { throw "-Env entries are NAME=value, not '$text'." }
            $knobs[$text.Substring(0, $at).Trim()] = $text.Substring($at + 1)
        }
    }
}

if ($ValidateOnly) {
    [pscustomobject]@{
        GamePath = $GamePath; GameSHA256 = $gameHash; GameVersion = $gameVersion
        TitleId = [string]$sfo['TITLE_ID']; AppVersion = $appVersion
        Headset = $Headset; NoOpenXr = [bool]$NoOpenXr; MirrorFps = $MirrorFps; FovPercent = $FovPercent; RenderPercent = $RenderPercent; EyeSize = ('{0}x{1}' -f (Get-EyeSize $RenderPercent).Width, (Get-EyeSize $RenderPercent).Height); Fps = $Fps; RateMode = $RateMode; SavedFps = $fpsToSave; SavedRateMode = $rateModeToSave; WorldSize = $WorldSize; Seconds = $Seconds; SkipIntro = $SkipIntro
        Executable = $exe; ExecutablePresent = (Test-Path -LiteralPath $exe -PathType Leaf)
        ConfigPath = $configPath; ConfigPresent = (Test-Path -LiteralPath $configPath -PathType Leaf)
        Environment = $knobs
    }
    return
}

if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) { throw "Missing emulator: $exe. Build shadps4.exe from C:\Games\Farpoint-src (branch ssuvr) and copy it there." }
$running = @(Get-CimInstance Win32_Process -Filter "Name = 'shadps4.exe'" -ErrorAction SilentlyContinue)
if ($running.Count -gt 0) {
    foreach ($other in $running) { Write-Host ("Running shadps4.exe PID {0}: {1}" -f $other.ProcessId, $other.CommandLine) }
    throw "Another shadps4.exe is running (see above). Close it before starting $title."
}

# Emulator profile: pc-vr\user is the portable user directory of shadps4.exe (it runs from pc-vr).
New-Item -ItemType Directory -Path $userDir -Force | Out-Null
if (-not (Test-Path -LiteralPath $configPath -PathType Leaf)) {
    if (-not (Test-Path -LiteralPath $configDefault -PathType Leaf)) { throw "Missing $configDefault." }
    Copy-Item -LiteralPath $configDefault -Destination $configPath
}
$config = Get-Content -LiteralPath $configPath -Raw | ConvertFrom-Json
# DualShock only (no special pad), and offline; everything else stays as the user set it.
Set-ConfigValue $config 'Input' 'use_special_pad' $false
Set-ConfigValue $config 'Network' 'connected_to_network' $false
Set-ConfigValue $config 'Network' 'shad_net_enabled' $false
# Profiles the emulator has not migrated yet keep the network keys under General.
foreach ($name in @('connected_to_network', 'shad_net_enabled')) {
    if ($config.PSObject.Properties['General'] -and $config.General -and $config.General.PSObject.Properties[$name]) { $config.General.$name = $false }
}
# A scene above 300% needs the bigger GPU heap (patch ssuvr-heap, written below) and emulator memory to
# back it: 2 GB more direct memory than a retail PS4's 4.5 GB.
$needsBigHeap = $RenderPercent -gt 300
Set-ConfigValue $config 'General' 'extra_dmem_in_mbytes' $(if ($needsBigHeap) { 2048 } else { 0 })
Write-Utf8 $configPath ($config | ConvertTo-Json -Depth 30)
Write-Utf8 $settingsFile ([ordered]@{ GamePath = $GamePath; Headset = $Headset; MirrorFps = $MirrorFps; FovPercent = $FovPercent; RenderPercent = $RenderPercent; Fps = $fpsToSave; RateMode = $rateModeToSave; WorldSize = $WorldSize; SkipIntro = $SkipIntro } | ConvertTo-Json)

# Eye render target size: the game builds its VR views from one packed constant, width | height << 32
# (0x438000003c0 = 960x1080), loaded by a movabs at 0x55782e. The memory patch below replaces that
# 8-byte immediate (vaddr 0x557830 + 0x400000). It is written on every start so the choice always matches.
$eye = Get-EyeSize $RenderPercent
$eyeHex = ('{0:x8}{1:x8}' -f $eye.Width, $eye.Height) -replace '(..)(..)(..)(..)', '$4$3$2$1'
$resDir = Join-Path (Join-Path $userDir 'patches') 'ssuvr-res'
New-Item -ItemType Directory -Path $resDir -Force | Out-Null
Write-Utf8 (Join-Path $resDir 'files.json') ('{"CUSA05951.xml": ["CUSA05951"]}' + "`n")
$resEnabled = if ($RenderPercent -eq 100) { 'false' } else { 'true' }
Write-Utf8 (Join-Path $resDir 'CUSA05951.xml') (@(
    '<?xml version="1.0" encoding="utf-8"?>'
    '<Patch>'
    '  <!-- Written by launch-ssuvr.ps1 from its RenderPercent setting; edits here are overwritten on the next start. -->'
    ('  <Metadata Name="Eye render targets {0}x{1} ({2}%)" AppVer="01.03" AppElf="eboot.bin" isEnabled="{3}">' -f $eye.Width, $eye.Height, $RenderPercent, $resEnabled)
    '    <PatchList>'
    ('      <Line Type="bytes" Address="00957830" Value="{0}"/>' -f $eyeHex.ToUpper())
    '    </PatchList>'
    '  </Metadata>'
    '</Patch>'
) -join "`n")

# Scene resolution: the game draws its whole 3D scene into one side-by-side buffer sized by its own HMD
# config file (/app0/Plan18.HMDconfig: DISPLAY_SCREEN_RESOLUTION 1920 1080 and WINDOW_DIMENSIONS
# 0 960 0 1080, i.e. 960x1080 an eye on a PSVR) and then stretches that into the eye pictures above.
# The file is rewritten on every start so the scene is drawn at the eye picture size. The game's own
# copy is kept once as pc-vr\Plan18.HMDconfig.original.
$maxScenePercent = 400
$scenePercent = [Math]::Min($RenderPercent, $maxScenePercent)

# GPU heap: engine init (sub_555a30) allocates one "Shared Video" heap of 0xA6800000 bytes (2.66 GB) that
# the scene targets, mesh blocks and the streamer share. At 400% (7680x4320 with 8 depth samples) the
# targets exhaust it and the game asserts in Mesh_Block_Manager_PS4.cpp / Streamer_Impl.cpp. The patch
# raises it to 0xF0000000 (3.75 GB; the size is a 32-bit field) at the allocate, map, print and store
# sites. Left alone on purpose: mov ecx,0xa6800000 at 0x555ce6, which only builds the mask 0xFFFFFFFF.
$heapDir = Join-Path (Join-Path $userDir 'patches') 'ssuvr-heap'
New-Item -ItemType Directory -Path $heapDir -Force | Out-Null
Write-Utf8 (Join-Path $heapDir 'files.json') ('{"CUSA05951.xml": ["CUSA05951"]}' + "`n")
Write-Utf8 (Join-Path $heapDir 'CUSA05951.xml') (@(
    '<?xml version="1.0" encoding="utf-8"?>'
    '<Patch>'
    '  <!-- Written by launch-ssuvr.ps1: enabled only for a 400% scene, together with extra_dmem_in_mbytes = 2048. -->'
    ('  <Metadata Name="Shared Video heap 3.75 GB" AppVer="01.03" AppElf="eboot.bin" isEnabled="{0}">' -f $(if ($needsBigHeap) { 'true' } else { 'false' }))
    '    <PatchList>'
    '      <Line Type="bytes" Address="00955BFB" Value="000000F0"/>'
    '      <Line Type="bytes" Address="00955C2C" Value="000000F0"/>'
    '      <Line Type="bytes" Address="00955CB2" Value="000000F0"/>'
    '      <Line Type="bytes" Address="00955ED3" Value="000000F0"/>'
    '    </PatchList>'
    '  </Metadata>'
    '</Patch>'
) -join "`n")

# Frame rate: the game's frame timer (the object at [0x9b6250], made in sub_426bc0) holds the fixed update
# rate and the frame cap. The HMD config loader (sub_25b710) sets both once with set_rate(59.94), loading
# 59.94 from 0x5b6488 at 0x25ca3f, and level starts reset the cap to the update rate. The main loop
# (sub_426e10) runs one update of 1/rate per frame and then waits out the cap. The emulator now sets the
# rate itself (SHADPS4_SSU_FPS, any rate, following the headset if asked), so this old 120-only patch is
# always written disabled (an older launcher may have left it enabled).
$fpsDir = Join-Path (Join-Path $userDir 'patches') 'ssuvr-fps'
New-Item -ItemType Directory -Path $fpsDir -Force | Out-Null
Write-Utf8 (Join-Path $fpsDir 'files.json') ('{"CUSA05951.xml": ["CUSA05951"]}' + "`n")
Write-Utf8 (Join-Path $fpsDir 'CUSA05951.xml') (@(
    '<?xml version="1.0" encoding="utf-8"?>'
    '<Patch>'
    '  <!-- Written by launch-ssuvr.ps1, always disabled (SHADPS4_SSU_FPS does this now); edits here are overwritten on the next start. -->'
    '  <Metadata Name="Game frame rate 120" AppVer="01.03" AppElf="eboot.bin" isEnabled="false">'
    '    <PatchList>'
    '      <!-- 0x25ca3f vmovss xmm0,[rip+0x359a41] (59.94 at 0x5b6488) -> [rip+0x359c95] (120.0 at 0x5b66dc) -->'
    '      <Line Type="bytes" Address="0065CA43" Value="959C3500"/>'
    '    </PatchList>'
    '  </Metadata>'
    '</Patch>'
) -join "`n")
$scene = Get-EyeSize $scenePercent
$hmdConfigPath = Join-Path (Split-Path $GamePath -Parent) 'Plan18.HMDconfig'
$hmdOriginal = Join-Path $PSScriptRoot 'Plan18.HMDconfig.original'
if (-not (Test-Path -LiteralPath $hmdOriginal) -and (Test-Path -LiteralPath $hmdConfigPath)) {
    Copy-Item -LiteralPath $hmdConfigPath -Destination $hmdOriginal
}
if (Test-Path -LiteralPath $hmdOriginal) {
    $hmdText = [IO.File]::ReadAllText($hmdOriginal)
    $hmdText = [regex]::Replace($hmdText, '(?m)^(DISPLAY_SCREEN_RESOLUTION\s+)\d+\s+\d+', ('${1}' + ('{0} {1}' -f (2 * $scene.Width), $scene.Height)))
    $hmdText = [regex]::Replace($hmdText, '(?m)^(WINDOW_DIMENSIONS\s+)0\s+\d+\s+0\s+\d+', ('${1}' + ("0`t{0}`t0`t{1}" -f $scene.Width, $scene.Height)))
    Write-Utf8 $hmdConfigPath $hmdText
} else {
    Write-Warning "No $hmdConfigPath in the game folder; the scene stays at the game's own size."
}

# /app0/HMD.config (the game's VR globe and camera radii) stays the game's own. An earlier launcher
# (2026-11-07) rewrote its STEREO_SEPARATION for the world size; if it kept a copy of the original, put
# that back.
$gameConfigPath = Join-Path (Split-Path $GamePath -Parent) 'HMD.config'
$gameConfigOriginal = Join-Path $PSScriptRoot 'HMD.config.original'
if ((Test-Path -LiteralPath $gameConfigOriginal) -and (Test-Path -LiteralPath $gameConfigPath)) {
    Copy-Item -LiteralPath $gameConfigOriginal -Destination $gameConfigPath -Force
}

foreach ($name in @($knobs.Keys)) {
    $value = [string]$knobs[$name]
    if ($value -eq '') { [Environment]::SetEnvironmentVariable($name, $null, 'Process') }
    else { [Environment]::SetEnvironmentVariable($name, $value, 'Process') }
}

$logRoot = Join-Path $userDir 'log'
New-Item -ItemType Directory -Path $logRoot -Force | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss-fff'
$priorLog = Join-Path $logRoot 'shad_log.txt'
if (Test-Path -LiteralPath $priorLog) {
    Copy-Item -LiteralPath $priorLog -Destination (Join-Path $logRoot "previous-$stamp.txt")
}
$session = [ordered]@{
    Started = (Get-Date).ToString('o'); Title = $title; TitleId = $titleId
    Executable = $exe; SHA256 = (Get-Sha256Hex $exe)
    GamePath = $GamePath; GameSHA256 = $gameHash; GameVersion = $gameVersion; AppVersion = $appVersion
    Headset = $Headset; NoOpenXr = [bool]$NoOpenXr; FovPercent = $FovPercent; RenderPercent = $RenderPercent; EyeSize = ('{0}x{1}' -f $eye.Width, $eye.Height); SceneSize = ('{0}x{1} an eye' -f $scene.Width, $scene.Height); Fps = $Fps; RateMode = $RateMode; WorldSize = $WorldSize; MirrorFps = $MirrorFps; Seconds = $Seconds; SkipIntro = $SkipIntro
    Environment = $knobs
}
Write-Utf8 (Join-Path $logRoot "session-$stamp.json") ($session | ConvertTo-Json -Depth 5)
$stdoutPath = Join-Path $logRoot "console-$stamp.stdout.txt"
$stderrPath = Join-Path $logRoot "console-$stamp.stderr.txt"
$process = Start-Process -FilePath $exe -WorkingDirectory $PSScriptRoot -ArgumentList @('-g', ('"' + $GamePath + '"')) -RedirectStandardOutput $stdoutPath -RedirectStandardError $stderrPath -PassThru
$processHandle = $process.Handle
Write-Host "$title started (PID $($process.Id), headset=$Headset, openxr=$($knobs['SHADPS4_OPENXR'])). Log: $priorLog"

$stopped = $false
if ($Seconds -gt 0) {
    if (-not $process.WaitForExit($Seconds * 1000)) {
        Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
        [void]$process.WaitForExit(15000)
        $stopped = $true
    }
} else {
    $process.WaitForExit()
}
$exitCode = $process.ExitCode
$exitHex = if ($null -ne $exitCode) { '0x{0:X8}' -f [BitConverter]::ToUInt32([BitConverter]::GetBytes([int]$exitCode), 0) } else { 'unavailable' }
$exit = [ordered]@{
    Ended = (Get-Date).ToString('o'); ProcessId = $process.Id; ExitCode = $exitCode; ExitCodeHex = $exitHex
    StoppedByLauncher = $stopped; Seconds = $Seconds; StandardOutput = $stdoutPath; StandardError = $stderrPath; Log = $priorLog
}
Write-Utf8 (Join-Path $logRoot "exit-$stamp.json") ($exit | ConvertTo-Json)
if ($stopped) { Write-Host "$title stopped after $Seconds s. Log: $priorLog"; return }
if ($exitCode -ne 0) { throw "$title ended with code $exitCode ($exitHex). See $logRoot (shad_log.txt, console-$stamp.*.txt)." }
Write-Host "$title ended normally. Log: $priorLog"
