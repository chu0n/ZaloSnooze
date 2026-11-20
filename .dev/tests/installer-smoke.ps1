param([string]$InnoCompiler = (Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 6\ISCC.exe'))
$ErrorActionPreference = 'Stop'
$dev = Split-Path $PSScriptRoot -Parent
$root = Split-Path $dev -Parent
$version = [regex]::Match((Get-Content (Join-Path $root 'CMakeLists.txt') -Raw), 'project\(ZaloSnooze VERSION ([0-9]+\.[0-9]+\.[0-9]+)').Groups[1].Value
$id = 'ZaloSnooze.NativeTest.' + [guid]::NewGuid().ToString('N')
$temporary = Join-Path ([IO.Path]::GetTempPath()) $id
$directory = Join-Path $temporary 'installation [custom path]'
$programs = [Environment]::GetFolderPath('Programs')
$shortcutFolder = Join-Path $programs $id
$uninstallKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\' + $id + '_is1'
$installed = $false
$userConfig = Join-Path $env:LOCALAPPDATA 'ZaloSnooze\ZaloSnooze.ini'
$configHash = if (Test-Path -LiteralPath $userConfig) { (Get-FileHash -LiteralPath $userConfig).Hash } else { '' }
try {
    New-Item -ItemType Directory -Path $temporary | Out-Null
    & $InnoCompiler '/Q' ("/DAppVersion=$version") ("/DInstallerAppId=$id") ("/DInstallerAppName=$id") ("/DOutputDirectory=$temporary") (Join-Path $dev 'installer\ZaloSnooze.iss')
    if ($LASTEXITCODE -ne 0) { throw 'Test installer compilation failed.' }
    $setup = Join-Path $temporary "ZaloSnooze-$version-Setup.exe"
    $process = Start-Process -FilePath $setup -ArgumentList ('/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /CURRENTUSER /TASKS="" /DIR="' + $directory + '"') -PassThru -Wait
    try { if ($process.ExitCode -ne 0) { throw "Installer failed: $($process.ExitCode)" } } finally { $process.Dispose() }
    $installed = $true
    foreach ($file in @('ZaloSnooze.exe', 'installed.ini', 'unins000.exe', 'LICENSE.txt')) {
        if (-not (Test-Path -LiteralPath (Join-Path $directory $file))) { throw "Installed file missing: $file" }
    }
    if (-not (Test-Path -LiteralPath (Join-Path $shortcutFolder ($id + '.lnk')))) { throw 'Start Menu shortcut missing.' }
    $oldUninstallShortcut = Join-Path $shortcutFolder ('Uninstall ' + $id + '.lnk')
    if (Test-Path -LiteralPath $oldUninstallShortcut) { throw 'Installer created an unwanted uninstall shortcut.' }
    if (-not (Test-Path -LiteralPath $uninstallKey)) { throw 'Windows uninstall registration missing.' }
    if (Test-Path -LiteralPath (Join-Path $directory 'ZaloSnooze.ini')) { throw 'Installer wrote user config into its program folder.' }
    Set-Content -LiteralPath (Join-Path $directory 'unrelated.txt') -Value 'Do not remove this file.'
    $shell = New-Object -ComObject WScript.Shell
    $link = $shell.CreateShortcut($oldUninstallShortcut)
    $link.TargetPath = Join-Path $directory 'unins000.exe'
    $link.Save()
    [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($link)
    [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($shell)
    $process = Start-Process -FilePath $setup -ArgumentList ('/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /CURRENTUSER /TASKS="" /DIR="' + $directory + '"') -PassThru -Wait
    try { if ($process.ExitCode -ne 0) { throw "Upgrade failed: $($process.ExitCode)" } } finally { $process.Dispose() }
    if (Test-Path -LiteralPath $oldUninstallShortcut) { throw 'Upgrade retained an old uninstall shortcut.' }
    if (-not (Test-Path -LiteralPath (Join-Path $directory 'unrelated.txt'))) { throw 'Upgrade removed an unrelated file.' }
    $process = Start-Process -FilePath (Join-Path $directory 'unins000.exe') -ArgumentList '/VERYSILENT /SUPPRESSMSGBOXES /NORESTART' -PassThru -Wait
    try { if ($process.ExitCode -ne 0) { throw "Uninstaller failed: $($process.ExitCode)" } } finally { $process.Dispose() }
    $installed = $false
    if (Test-Path -LiteralPath (Join-Path $directory 'ZaloSnooze.exe')) { throw 'Uninstaller left the application executable.' }
    if (-not (Test-Path -LiteralPath (Join-Path $directory 'unrelated.txt'))) { throw 'Uninstaller removed an unrelated file.' }
    if (Test-Path -LiteralPath $uninstallKey) { throw 'Uninstaller left its Windows registration.' }
    if (Test-Path -LiteralPath $shortcutFolder) { throw 'Uninstaller left its Start Menu folder.' }
    $afterHash = if (Test-Path -LiteralPath $userConfig) { (Get-FileHash -LiteralPath $userConfig).Hash } else { '' }
    if ($configHash -cne $afterHash) { throw 'Install/uninstall changed existing AppData configuration.' }
    Write-Host 'PASS: current-user install/upgrade, app-only Start Menu shortcut, old uninstall shortcut cleanup, Windows uninstall registration, uninstaller, unrelated-file preservation'
} finally {
    if ($installed) { Write-Warning "Test installation retained for investigation: $directory" }
    elseif (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary -Recurse -Force }
}
$global:LASTEXITCODE = 0
