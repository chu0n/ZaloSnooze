param([string]$Msys2Root)
$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
if (-not $Msys2Root) {
    $gcc = Get-Command g++.exe -ErrorAction Stop
    $Msys2Root = Split-Path (Split-Path (Split-Path $gcc.Source -Parent) -Parent) -Parent
}
$renderer = Join-Path $Msys2Root 'ucrt64\bin\rsvg-convert.exe'
if (-not (Test-Path -LiteralPath $renderer)) { throw 'Install mingw-w64-ucrt-x86_64-librsvg to regenerate icons. Normal builds use the checked-in icons.' }
$png = Join-Path $root 'docs\assets\logo-transparent.png'
& $renderer (Join-Path $root 'docs\assets\logo-transparent.svg') -o $png
if ($LASTEXITCODE -ne 0) { throw 'SVG rendering failed.' }
$frames = [Collections.Generic.List[byte[]]]::new()
$sizes = @(16, 20, 24, 32, 40, 48, 64, 96, 128, 256)
$temporary = Join-Path ([IO.Path]::GetTempPath()) ('ZaloSnooze-icons-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $temporary | Out-Null
try {
    foreach ($size in $sizes) {
        $frame = Join-Path $temporary "$size.png"
        & $renderer (Join-Path $root 'docs\assets\logo-transparent.svg') -w $size -h $size -o $frame
        if ($LASTEXITCODE -ne 0) { throw "Rendering $size pixel icon failed." }
        $frames.Add([IO.File]::ReadAllBytes($frame))
    }
} finally { Remove-Item -LiteralPath $temporary -Recurse -Force }
$file = [IO.File]::Create((Join-Path $root 'docs\assets\logo.ico'))
$writer = [IO.BinaryWriter]::new($file)
try {
    $writer.Write([uint16]0); $writer.Write([uint16]1); $writer.Write([uint16]$sizes.Count)
    $offset = 6 + 16 * $sizes.Count
    for ($i = 0; $i -lt $sizes.Count; $i++) {
        $size = if ($sizes[$i] -eq 256) { 0 } else { $sizes[$i] }
        $writer.Write([byte]$size); $writer.Write([byte]$size); $writer.Write([uint16]0)
        $writer.Write([uint16]1); $writer.Write([uint16]32)
        $writer.Write([uint32]$frames[$i].Length); $writer.Write([uint32]$offset)
        $offset += $frames[$i].Length
    }
    foreach ($frame in $frames) { $writer.Write([byte[]]$frame) }
} finally { $writer.Dispose() }
Write-Host 'Generated transparent PNG and multi-size ICO.'
