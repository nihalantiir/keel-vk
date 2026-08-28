$ErrorActionPreference = "Stop"

$RootDir = Split-Path -Parent $PSScriptRoot
$BuildDir = Join-Path $RootDir "build"

if (Test-Path $BuildDir) {
    Remove-Item -Recurse -Force $BuildDir
    Write-Host "Removed $BuildDir"
} else {
    Write-Host "Nothing to clean."
}

# imgui.ini and pipeline_cache.bin write next to wherever the exe was run
# from, not necessarily build/<preset>/bin - sweep stray copies at repo
# root, the common case when the exe is run with the repo root as cwd.
foreach ($name in @("imgui.ini", "pipeline_cache.bin")) {
    $path = Join-Path $RootDir $name
    if (Test-Path $path) {
        Remove-Item -Force $path
        Write-Host "Removed $path"
    }
}
Get-ChildItem -Path $RootDir -Filter "*.ilk" -File -ErrorAction SilentlyContinue | ForEach-Object {
    Remove-Item -Force $_.FullName
    Write-Host "Removed $($_.FullName)"
}
$PdbDir = Join-Path $RootDir "pdb"
if (Test-Path $PdbDir) {
    Remove-Item -Recurse -Force $PdbDir
    Write-Host "Removed $PdbDir"
}
