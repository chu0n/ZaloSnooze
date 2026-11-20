[CmdletBinding()]
param([string]$Msys2Root)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'tools\build-common.ps1')
$tools = Get-NativeTools $Msys2Root
$oldPath = $env:PATH
$oldCrossPid = $env:SNOOZE_TEST_CROSS_PID
try {
    $env:PATH = $tools.Bin + ';' + $oldPath
    $env:SNOOZE_TEST_CROSS_PID = [string]$PID
    & (Join-Path $tools.Bin 'ctest.exe') --test-dir (Join-Path $PSScriptRoot 'build') --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw 'Native tests failed.' }
    & (Join-Path $PSScriptRoot 'tests\windows-smoke.ps1')
} finally { $env:PATH = $oldPath; $env:SNOOZE_TEST_CROSS_PID = $oldCrossPid }
