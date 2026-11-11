import re
p = r'C:\Games\SuperStardustUltraVR\pc-vr\launch-ssuvr.ps1'
s = open(p, encoding='utf-8').read()


def rep(old, new, count=1):
    global s
    assert s.count(old) >= 1, old[:60]
    s = s.replace(old, new, count)


# 1. parameters
rep(r'''    [ValidateRange(60,120)][int]$FovPercent = 100,
''', r'''    # Percent of the headset's own field of view the game draws (100 = the whole view; a PSVR's is ~100 of 110 deg).
    [ValidateRange(60,120)][int]$FovPercent = 100,
    # Eye render target size in percent of the PSVR's 960x1080 an eye (200 = 1920x2160), through the memory
    # patch the launcher writes to user\patches\ssuvr-res\. 100 leaves the game at its original size.
    [ValidateSet(100,150,200,250,300)][int]$RenderPercent = 200,
''')
# 2. saved settings load
rep(r'''        if (-not $PSBoundParameters.ContainsKey('FovPercent') -and $saved.FovPercent -ge 60 -and $saved.FovPercent -le 120) { $FovPercent = [int]$saved.FovPercent }
''', r'''        if (-not $PSBoundParameters.ContainsKey('FovPercent') -and $saved.FovPercent -ge 60 -and $saved.FovPercent -le 120) { $FovPercent = [int]$saved.FovPercent }
        if (-not $PSBoundParameters.ContainsKey('RenderPercent') -and $saved.RenderPercent -in $renderChoices) { $RenderPercent = [int]$saved.RenderPercent }
''')
rep(r'''$headsetChoices = @('on', 'auto', 'off')
''', r'''$headsetChoices = @('on', 'auto', 'off')
# Eye render target sizes: percent of 960x1080, widths rounded up to 16 and heights to 8 pixels.
$renderChoices = @(100, 150, 200, 250, 300)
function Get-EyeSize([int]$percent) {
    $w = [int][Math]::Ceiling(960.0 * $percent / 100 / 16) * 16
    $h = [int][Math]::Ceiling(1080.0 * $percent / 100 / 8) * 8
    $note = if ($percent -eq 100) { ' (original)' } elseif ($percent -eq 200) { ' (recommended)' } else { '' }
    @{ Width = $w; Height = $h; Text = "$percent% - ${w}x${h} an eye$note" }
}
''')
# 3. GUI: taller form, resolution row, shifted rows, new texts
rep("$form.ClientSize = New-Object System.Drawing.Size(620,392)", "$form.ClientSize = New-Object System.Drawing.Size(620,436)")
rep(r'''    $fovLabel = New-Object System.Windows.Forms.Label
    $fovLabel.Text = "Rendered FOV: $FovPercent%"; $fovLabel.SetBounds(18,187,580,20); $form.Controls.Add($fovLabel)
''', r'''    $renderLabel = New-Object System.Windows.Forms.Label
    $renderLabel.Text = 'Render resolution (the eye pictures; higher is sharper, costs GPU time)'; $renderLabel.SetBounds(18,180,580,20); $form.Controls.Add($renderLabel)
    $renderCombo = New-Object System.Windows.Forms.ComboBox
    $renderCombo.DropDownStyle = 'DropDownList'; $renderCombo.SetBounds(18,204,360,25)
    foreach ($choice in $renderChoices) { [void]$renderCombo.Items.Add((Get-EyeSize $choice).Text) }
    $renderCombo.SelectedIndex = [Array]::IndexOf($renderChoices, $RenderPercent); $form.Controls.Add($renderCombo)
    $fovLabel = New-Object System.Windows.Forms.Label
    $fovLabel.Text = "Rendered FOV: $FovPercent% of the headset's view"; $fovLabel.SetBounds(18,239,580,20); $form.Controls.Add($fovLabel)
''')
rep("$fovSlider.Value = $FovPercent; $fovSlider.SetBounds(18,210,582,45)", "$fovSlider.Value = $FovPercent; $fovSlider.SetBounds(18,262,582,45)")
rep('''$fovSlider.Add_ValueChanged({ $fovLabel.Text = "Rendered FOV: $($fovSlider.Value)%" })''',
    '''$fovSlider.Add_ValueChanged({ $fovLabel.Text = "Rendered FOV: $($fovSlider.Value)% of the headset's view" })''')
rep("$fovHelp.Text = '60% (narrower)                 100% (original)                 120% (wider)'",
    "$fovHelp.Text = '60% (narrower, sharper)          100% (the whole headset view)          120% (wider)'")
rep("$fovHelp.SetBounds(18,257,580,20)", "$fovHelp.SetBounds(18,309,580,20)")
rep("$skipBox.Text = 'Skip the intro: the video ends at once and X is pressed for you through the legal, calibration and title prompts'",
    "$skipBox.Text = 'Skip the intro: boot straight to the title screen (memory patch; logos, legal text and calibrate prompt skipped)'")
rep("$skipBox.SetBounds(18,280,582,22)", "$skipBox.SetBounds(18,332,582,22)")
rep("$info.SetBounds(18,304,580,40)", "$info.SetBounds(18,356,580,40)")
rep("$play.Text = 'Start'; $play.SetBounds(455,345,145,32)", "$play.Text = 'Start'; $play.SetBounds(455,397,145,32)")
rep(r'''    $FovPercent = $fovSlider.Value
''', r'''    $FovPercent = $fovSlider.Value
    $RenderPercent = $renderChoices[$renderCombo.SelectedIndex]
''')
# 4. FOV knob
rep(r'''# Percent of a PlayStation VR's field of view the title is told to draw; 100 is the original.
$knobs['SHADPS4_VR_FOV'] = [string]$FovPercent
''', r'''# The title is told the headset's own field of view (a PSVR's 100 degrees would leave a border in a
# 110-degree headset); the game draws with whatever it is told. -FovPercent scales it; 100 is the whole view.
$knobs['SHADPS4_VR_FOV'] = [string]$FovPercent
$knobs['SHADPS4_VR_FOV_OF'] = 'headset'
''')
# 5. ValidateOnly / settings / session
rep("Headset = $Headset; NoOpenXr = [bool]$NoOpenXr; MirrorFps = $MirrorFps; FovPercent = $FovPercent; Seconds = $Seconds; SkipIntro = $SkipIntro\n        Executable = $exe; ExecutablePresent",
    "Headset = $Headset; NoOpenXr = [bool]$NoOpenXr; MirrorFps = $MirrorFps; FovPercent = $FovPercent; RenderPercent = $RenderPercent; EyeSize = ('{0}x{1}' -f (Get-EyeSize $RenderPercent).Width, (Get-EyeSize $RenderPercent).Height); Seconds = $Seconds; SkipIntro = $SkipIntro\n        Executable = $exe; ExecutablePresent")
rep("Write-Utf8 $settingsFile ([ordered]@{ GamePath = $GamePath; Headset = $Headset; MirrorFps = $MirrorFps; FovPercent = $FovPercent; SkipIntro = $SkipIntro } | ConvertTo-Json)",
    r'''Write-Utf8 $settingsFile ([ordered]@{ GamePath = $GamePath; Headset = $Headset; MirrorFps = $MirrorFps; FovPercent = $FovPercent; RenderPercent = $RenderPercent; SkipIntro = $SkipIntro } | ConvertTo-Json)

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
) -join "`n")''')
rep("Headset = $Headset; NoOpenXr = [bool]$NoOpenXr; FovPercent = $FovPercent; MirrorFps = $MirrorFps; Seconds = $Seconds; SkipIntro = $SkipIntro\n    Environment = $knobs",
    "Headset = $Headset; NoOpenXr = [bool]$NoOpenXr; FovPercent = $FovPercent; RenderPercent = $RenderPercent; EyeSize = ('{0}x{1}' -f $eye.Width, $eye.Height); MirrorFps = $MirrorFps; Seconds = $Seconds; SkipIntro = $SkipIntro\n    Environment = $knobs")
open(p, 'w', encoding='utf-8').write(s)
print('launcher edited')
