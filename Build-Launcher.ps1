param([switch]$UseInstalledDependencies, [switch]$Install)
$ErrorActionPreference = 'Stop'
if (Get-Process LCSrecomp -ErrorAction SilentlyContinue) { throw 'LCSrecomp läuft. Bitte vor dem Build schließen.' }
$env:CARGO_TARGET_DIR = Join-Path $PSScriptRoot 'build/launcher'
Push-Location (Join-Path $PSScriptRoot 'launcher')
try {
    if (!$UseInstalledDependencies) {
        & npm.cmd ci --prefer-offline
        if ($LASTEXITCODE -ne 0) { throw 'npm ci fehlgeschlagen.' }
    }
    & npm.cmd run tauri -- build --no-bundle
    if ($LASTEXITCODE -ne 0) { throw 'Tauri-Build fehlgeschlagen.' }
    $out = Join-Path $env:CARGO_TARGET_DIR 'release'
    if ($Install) {
        & (Join-Path $PSScriptRoot 'tools/scripts/Install-Build.ps1') -Source $out -Destination (Join-Path $PSScriptRoot 'bin/launcher') -Executable 'lcs-recomp-launcher.exe' -ProcessName 'lcs-recomp-launcher'
    } else { Get-Item -LiteralPath (Join-Path $out 'lcs-recomp-launcher.exe') }
} finally { Pop-Location }

