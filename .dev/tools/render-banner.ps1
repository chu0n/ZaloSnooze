param([string]$Msys2Root)
$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
if (-not $Msys2Root) {
    $gcc = Get-Command g++.exe -ErrorAction Stop
    $Msys2Root = Split-Path (Split-Path (Split-Path $gcc.Source -Parent) -Parent) -Parent
}
$renderer = Join-Path $Msys2Root 'ucrt64\bin\rsvg-convert.exe'
if (-not (Test-Path -LiteralPath $renderer)) { throw 'Install mingw-w64-ucrt-x86_64-librsvg to regenerate artwork. Normal builds use checked-in bitmaps.' }
$assets = Join-Path $root 'docs\assets'
$template = Get-Content -LiteralPath (Join-Path $assets 'banner.svg') -Raw -Encoding UTF8
$logo = [Convert]::ToBase64String([IO.File]::ReadAllBytes((Join-Path $assets 'logo-transparent.svg')))
$template = $template.Replace('href="logo-transparent.svg"', ('href="data:image/svg+xml;base64,' + $logo + '"'))
$temporary = Join-Path ([IO.Path]::GetTempPath()) ('ZaloSnooze-banner-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $temporary | Out-Null
Add-Type -AssemblyName System.Drawing
try {
    foreach ($language in @('en','vi')) {
        $svg = $template
        if ($language -eq 'vi') {
            $svg = $svg.Replace('A LITTLE WINDOWS HELPER', 'TRỢ LÝ NHỎ CHO WINDOWS').Replace('Zalo snoozes. You stay focused.', 'Zalo ngủ. Bạn cứ tập trung.').Replace('One sleepy click at sign-in.', 'Mở Zalo khi bạn cần.').Replace('a sleepy chat window', 'cửa sổ chat đang ngủ').Replace('Zalo is still running.', 'Zalo vẫn đang chạy.').Replace('Just tucked into the tray.', 'Thu mình xuống khay.')
            # Comic Sans lacks some Vietnamese glyphs; keep the wordmark, use full-coverage text elsewhere.
            [xml]$localized = $svg.Replace('Comic Sans MS, Trebuchet MS, cursive', 'Segoe UI, sans-serif')
            $localized.SelectSingleNode("//*[@id='wordmark']").SetAttribute('font-family', 'Comic Sans MS, Trebuchet MS, cursive')
            $svg = $localized.OuterXml
        }
        $source = Join-Path $temporary "$language.svg"
        [IO.File]::WriteAllText($source, $svg, [Text.UTF8Encoding]::new($false))
        & $renderer $source -o (Join-Path $assets "banner.$language.png")
        if ($LASTEXITCODE -ne 0) { throw 'Documentation banner rendering failed.' }
        [xml]$compact = $svg
        $compact.DocumentElement.SetAttribute('height', '180')
        $compact.DocumentElement.SetAttribute('viewBox', '0 0 1600 180')
        foreach ($entry in @{
            background='scale(1 .3214285714)'; eyebrow='translate(0 -28) scale(.75)';
            wordmark='translate(0 -39) scale(.65)'; slogan='translate(0 -80) scale(.75)';
            window='translate(570 -22) scale(.45)'; tray='translate(640 -160) scale(.62) rotate(-3 1248 462)';
            trail='translate(570 -52) scale(.45)'; 'star-right'='translate(-135 -145) scale(.65)';
            'star-middle'='translate(275 80) scale(.65)'; accent='translate(320 -150) scale(.5)'
        }.GetEnumerator()) {
            $compact.SelectSingleNode("//*[@id='$($entry.Key)']").SetAttribute('transform', $entry.Value)
        }
        foreach ($pattern in @('dots','big-dots')) { $compact.SelectSingleNode("//*[@id='$pattern']").SetAttribute('patternTransform', 'scale(1 3.1111111111)') }
        [IO.File]::WriteAllText($source, $compact.OuterXml, [Text.UTF8Encoding]::new($false))
        $png = Join-Path $temporary "$language.png"
        & $renderer $source -o $png
        if ($LASTEXITCODE -ne 0) { throw 'Banner rendering failed.' }
        $image = [Drawing.Image]::FromFile($png)
        try {
            $bitmap = [Drawing.Bitmap]::new($image.Width, $image.Height, [Drawing.Imaging.PixelFormat]::Format24bppRgb)
            try {
                $graphics = [Drawing.Graphics]::FromImage($bitmap)
                try { $graphics.DrawImageUnscaled($image, 0, 0) } finally { $graphics.Dispose() }
                $bitmap.Save((Join-Path $assets "app-banner.$language.bmp"), [Drawing.Imaging.ImageFormat]::Bmp)
            } finally { $bitmap.Dispose() }
        } finally { $image.Dispose() }
    }
} finally { Remove-Item -LiteralPath $temporary -Recurse -Force }
Write-Host 'Generated embedded English/Vietnamese banners.'
