[CmdletBinding()]
param([string]$Msys2Root, [string]$ProjectUrl, [switch]$AppOnly)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'tools\build-common.ps1')
$tools = Get-NativeTools $Msys2Root
$root = Split-Path $PSScriptRoot -Parent
$oldPath = $env:PATH
try {
    $env:PATH = $tools.Bin + ';' + $oldPath
    if (-not $ProjectUrl -and $env:GITHUB_REPOSITORY) { $ProjectUrl = 'https://github.com/' + $env:GITHUB_REPOSITORY }
    if (-not $ProjectUrl) { $ProjectUrl = 'https://github.com/chu0n/ZaloSnooze' }
    if ($ProjectUrl -and $ProjectUrl -notmatch '^https://github\.com/[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+/?$') { throw 'ProjectUrl must be an HTTPS GitHub repository URL.' }
    $ProjectUrl = ([string]$ProjectUrl).TrimEnd('/')
    $compiler = (Join-Path $tools.Bin 'g++.exe').Replace('\', '/')
    $resources = (Join-Path $tools.Bin 'windres.exe').Replace('\', '/')
    $ninja = (Join-Path $tools.Bin 'ninja.exe').Replace('\', '/')
    & $tools.CMake -S $root -B (Join-Path $PSScriptRoot 'build') -G Ninja '-DCMAKE_BUILD_TYPE=Release' ('-DCMAKE_CXX_COMPILER=' + $compiler) ('-DCMAKE_RC_COMPILER=' + $resources) ('-DCMAKE_MAKE_PROGRAM=' + $ninja) ('-DZALOSNOOZE_PROJECT_URL=' + $ProjectUrl)
    if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed.' }
    $target = if ($AppOnly) { @('--target', 'ZaloSnooze') } else { @() }
    & $tools.CMake --build (Join-Path $PSScriptRoot 'build') --parallel 2 @target
    if ($LASTEXITCODE -ne 0) { throw 'Native build failed.' }
    Get-Item (Join-Path $PSScriptRoot 'build\ZaloSnooze.exe') | Select-Object FullName, Length
} finally { $env:PATH = $oldPath }
