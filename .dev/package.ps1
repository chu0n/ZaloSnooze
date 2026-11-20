[CmdletBinding()]
param([string]$Msys2Root, [string]$InnoCompiler, [string]$Tag)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'tools\build-common.ps1')
$tools = Get-NativeTools $Msys2Root
$root = Split-Path $PSScriptRoot -Parent
$project = Get-Content -LiteralPath (Join-Path $root 'CMakeLists.txt') -Raw
$version = [regex]::Match($project, 'project\(ZaloSnooze VERSION ([0-9]+\.[0-9]+\.[0-9]+)').Groups[1].Value
if (-not $version) { throw 'Cannot read CMake project version.' }
if ($Tag -and $Tag -cne "v$version") { throw "Tag must match v$version." }
$exe = Join-Path $PSScriptRoot 'build\ZaloSnooze.exe'
if (-not (Test-Path -LiteralPath $exe)) { throw 'Run build.ps1 first.' }
$info = [Diagnostics.FileVersionInfo]::GetVersionInfo($exe)
if ($info.ProductVersion -cne $version) { throw 'Executable version differs from source. Rebuild first.' }
$imports = & (Join-Path $tools.Bin 'objdump.exe') -p $exe
if ($LASTEXITCODE -ne 0 -or ($imports -join "`n") -notmatch 'pei-i386') { throw 'Portable executable must be x86.' }
$dlls = @($imports | Select-String 'DLL Name:\s*(\S+)' | ForEach-Object { $_.Matches[0].Groups[1].Value.ToLowerInvariant() })
$allowed = @('advapi32.dll','comctl32.dll','comdlg32.dll','dwmapi.dll','gdi32.dll','kernel32.dll','msvcrt.dll','ole32.dll','oleaut32.dll','shell32.dll','shlwapi.dll','user32.dll','version.dll','bcrypt.dll','ntdll.dll')
foreach ($dll in $dlls) { if ($allowed -notcontains $dll -and $dll -notlike 'api-ms-win-*') { throw "Unexpected runtime DLL dependency: $dll" } }
if (-not $InnoCompiler) {
    $found = Get-Command ISCC.exe -ErrorAction SilentlyContinue
    if ($found) { $InnoCompiler = $found.Source }
    else {
        foreach ($path in @((Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 6\ISCC.exe'), 'C:\Program Files (x86)\Inno Setup 6\ISCC.exe')) {
            if (Test-Path -LiteralPath $path) { $InnoCompiler = $path; break }
        }
    }
}
if (-not $InnoCompiler) { throw 'Inno Setup compiler missing. See docs/DEVELOPMENT.md or pass -InnoCompiler.' }
$release = Join-Path $PSScriptRoot 'release'
New-Item -ItemType Directory -Path $release -Force | Out-Null
& $InnoCompiler ('/DAppVersion=' + $version) (Join-Path $PSScriptRoot 'installer\ZaloSnooze.iss')
if ($LASTEXITCODE -ne 0) { throw 'Setup compilation failed.' }
$temporary = Join-Path $release ('.portable-' + [guid]::NewGuid().ToString('N'))
$zip = Join-Path $release "ZaloSnooze-$version-Portable-x86.zip"
try {
    $folder = Join-Path $temporary 'ZaloSnooze'
    New-Item -ItemType Directory -Path $folder -Force | Out-Null
    Copy-Item -LiteralPath $exe -Destination (Join-Path $folder 'ZaloSnooze.exe')
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'config\ZaloSnooze.ini') -Destination $folder
    Copy-Item -LiteralPath (Join-Path $root 'docs\PORTABLE.txt') -Destination (Join-Path $folder 'README.txt')
    Copy-Item -LiteralPath (Join-Path $root 'LICENSE') -Destination (Join-Path $folder 'LICENSE.txt')
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $staged = Join-Path $temporary 'portable.zip'
    [IO.Compression.ZipFile]::CreateFromDirectory($folder, $staged, [IO.Compression.CompressionLevel]::Optimal, $true)
    Move-Item -LiteralPath $staged -Destination $zip -Force
} finally { if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary -Recurse -Force } }
$assets = @((Join-Path $release "ZaloSnooze-$version-Setup.exe"), $zip)
foreach ($file in $assets) { Get-Item -LiteralPath $file | Select-Object Name, Length; Get-FileHash -LiteralPath $file -Algorithm SHA256 | Select-Object Hash }
Write-Host ('Runtime DLLs: ' + ($dlls -join ', '))
