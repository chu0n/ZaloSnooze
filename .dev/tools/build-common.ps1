function Get-NativeTools {
    param([string]$Msys2Root)
    if (-not $Msys2Root) {
        $gcc = Get-Command g++.exe -ErrorAction SilentlyContinue
        if ($gcc) { $Msys2Root = Split-Path (Split-Path (Split-Path $gcc.Source -Parent) -Parent) -Parent }
        elseif (Test-Path 'C:\msys64') { $Msys2Root = 'C:\msys64' }
    }
    if (-not $Msys2Root) { throw 'MSYS2 not found. Pass -Msys2Root or follow docs/DEVELOPMENT.md.' }
    $bin = Join-Path $Msys2Root 'mingw32\bin'
    foreach ($file in @('g++.exe', 'cmake.exe', 'ninja.exe', 'windres.exe', 'objdump.exe')) {
        if (-not (Test-Path -LiteralPath (Join-Path $bin $file))) { throw "Missing $file in $bin. Install the MINGW32 packages listed in docs/DEVELOPMENT.md." }
    }
    return @{ Bin = $bin; CMake = (Join-Path $bin 'cmake.exe') }
}
