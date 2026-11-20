$ErrorActionPreference = 'Stop'
$dev = Split-Path $PSScriptRoot -Parent
$root = Split-Path $dev -Parent
$version = [regex]::Match((Get-Content (Join-Path $root 'CMakeLists.txt') -Raw), 'project\(ZaloSnooze VERSION ([0-9]+\.[0-9]+\.[0-9]+)').Groups[1].Value
$zip = Join-Path $dev "release\ZaloSnooze-$version-Portable-x86.zip"
$setup = Join-Path $dev "release\ZaloSnooze-$version-Setup.exe"
if (-not (Test-Path -LiteralPath $setup)) { throw 'Setup release asset missing.' }
$temporary = Join-Path ([IO.Path]::GetTempPath()) ('ZaloSnooze-release-tests-' + [guid]::NewGuid().ToString('N'))
Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [IO.Compression.ZipFile]::OpenRead($zip)
try {
    $files = @($archive.Entries | Where-Object { $_.Length -gt 0 } | ForEach-Object { $_.FullName.Replace('\', '/') } | Sort-Object)
    $expected = @('ZaloSnooze/LICENSE.txt', 'ZaloSnooze/README.txt', 'ZaloSnooze/ZaloSnooze.exe', 'ZaloSnooze/ZaloSnooze.ini') | Sort-Object
    if (($files -join "`n") -cne ($expected -join "`n")) { throw ('Unexpected portable contents: ' + ($files -join ', ')) }
} finally { $archive.Dispose() }
try {
    [IO.Compression.ZipFile]::ExtractToDirectory($zip, $temporary)
    $folder = Join-Path $temporary 'ZaloSnooze'
    if ((Get-FileHash (Join-Path $folder 'ZaloSnooze.ini')).Hash -ne (Get-FileHash (Join-Path $dev 'config\ZaloSnooze.ini')).Hash) { throw 'Portable package contains modified/private configuration.' }
    if ((Get-FileHash (Join-Path $folder 'ZaloSnooze.exe')).Hash -ne (Get-FileHash (Join-Path $dev 'build\ZaloSnooze.exe')).Hash) { throw 'Portable package contains stale executable.' }
    & (Join-Path $PSScriptRoot 'windows-smoke.ps1') -Executable (Join-Path $folder 'ZaloSnooze.exe')
    $workflow = Get-Content -LiteralPath (Join-Path $root '.github\workflows\build.yml') -Raw
    if ($workflow -notmatch '(?m)^    concurrency:\s*$' -or
        $workflow -notmatch '(?m)^      group: release-\$\{\{ github\.ref_name \}\}\s*$' -or
        $workflow -notmatch '(?m)^      cancel-in-progress: false\s*$') {
        throw 'Release publication is not serialized per tag.'
    }
    $publication = [regex]::Match($workflow,'(?ms)^          \$assets = .*').Value -replace '(?m)^          ',''
    if (-not $publication) { throw 'Release publication script not found.' }
    $publication = [scriptblock]::Create($publication)
    $publicationDirectory = Join-Path $temporary 'publication'
    $assetDirectory = Join-Path $publicationDirectory 'release'
    New-Item -ItemType Directory -Path $assetDirectory | Out-Null
    Set-Content -LiteralPath (Join-Path $assetDirectory 'Setup.exe') -Value 'setup fixture'
    Set-Content -LiteralPath (Join-Path $assetDirectory 'Portable.zip') -Value 'portable fixture'
    $script:publicationCalls = [Collections.Generic.List[object]]::new()
    $script:releaseExists = $true
    $script:createFails = $false
    $script:editFails = $false
    function gh {
        $script:publicationCalls.Add([string[]]$args)
        $global:LASTEXITCODE = if (($args[1] -eq 'view' -and -not $script:releaseExists) -or
                                   ($args[1] -eq 'create' -and $script:createFails) -or
                                  ($args[1] -eq 'edit' -and $script:editFails)) { 1 } else { 0 }
    }
    $oldRepository = $env:GITHUB_REPOSITORY
    $oldRef = $env:GITHUB_REF_NAME
    $oldExit = $global:LASTEXITCODE
    Push-Location $publicationDirectory
    try {
        $env:GITHUB_REPOSITORY = 'test/repository'
        $env:GITHUB_REF_NAME = 'v0.0.0-test'
        $rejected = $false
        try { & $publication } catch { $rejected = $true }
        if (-not $rejected -or $script:publicationCalls.Count -ne 1) { throw 'An existing release must not be mutated.' }
        $script:publicationCalls.Clear()
        $script:releaseExists = $false
        & $publication
        if (($script:publicationCalls | ForEach-Object { $_[1] }) -join ',' -cne 'view,create,edit') { throw 'Publish must follow draft creation and upload.' }
        if (-not ($script:publicationCalls[1] -contains '--notes-file') -or
            -not ($script:publicationCalls[1] -contains '--draft') -or
            -not ($script:publicationCalls[2] -contains '--draft=false')) { throw 'Release did not stage notes and assets before publication.' }
        $notes = Get-Content -LiteralPath 'release-notes.txt' -Raw
        foreach ($file in Get-ChildItem -LiteralPath 'release' -File) {
            $hash = (Get-FileHash -LiteralPath $file.FullName).Hash.ToLowerInvariant()
            if (-not $notes.Contains("$hash  $($file.Name)")) { throw 'Published notes do not contain current asset hashes.' }
        }
        foreach ($failure in @('create','edit')) {
            $script:publicationCalls.Clear()
            $script:createFails = $failure -eq 'create'
            $script:editFails = $failure -eq 'edit'
            $rejected = $false
            try { & $publication } catch { $rejected = $true }
            if (-not $rejected) { throw "Release $failure failure was ignored." }
            if ($failure -eq 'create' -and $script:publicationCalls.Count -ne 2) { throw 'An incomplete draft was published.' }
        }
    } finally {
        Pop-Location
        $env:GITHUB_REPOSITORY = $oldRepository
        $env:GITHUB_REF_NAME = $oldRef
        $global:LASTEXITCODE = $oldExit
    }
    Write-Host 'PASS: immutable releases, draft-first publication, checksum notes, and failure handling (GitHub calls mocked)'
    Write-Host 'PASS: release asset names, exact ZIP contents, clean default configuration, executable hash, extracted-app GUI'
} finally { if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary -Recurse -Force } }
$global:LASTEXITCODE = 0
