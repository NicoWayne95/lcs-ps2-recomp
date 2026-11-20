param(
    [string]$Version = '0.2.0',
    [string]$LauncherExe,
    [string]$KitPath,
    [string]$OutputDirectory
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (!$LauncherExe) { $LauncherExe = Join-Path $root 'src-tauri/target/release/lcs-recomp-launcher.exe' }
if (!$KitPath) { $KitPath = Join-Path $root 'Source Code' }
if (!$OutputDirectory) { $OutputDirectory = Join-Path $root 'dist-installer' }
if (!(Test-Path -LiteralPath $LauncherExe -PathType Leaf)) { throw 'Launcher fehlt.' }
if (!(Test-Path -LiteralPath (Join-Path $KitPath 'kit.json') -PathType Leaf)) { throw 'Kit fehlt.' }
$zip = Join-Path $OutputDirectory "LCS-Recomp-Beta-$Version-portable.zip"
if (Test-Path -LiteralPath $zip) { throw "Paket existiert bereits: $zip" }
$stage = Join-Path $OutputDirectory ('portable-' + [guid]::NewGuid().ToString('N'))
$folder = Join-Path $stage 'LCS Recomp'
$game = Join-Path $folder 'game'
New-Item -ItemType Directory -Path $game | Out-Null
Copy-Item -LiteralPath $LauncherExe -Destination $folder
Copy-Item -LiteralPath $KitPath -Destination (Join-Path $folder 'Source Code') -Recurse
Copy-Item -LiteralPath (Join-Path $KitPath 'NOTICE.txt'),(Join-Path $KitPath 'LICENSE.txt'),(Join-Path $PSScriptRoot 'beta/BETA_README.txt') -Destination $folder
Set-Content -LiteralPath (Join-Path $game 'README.txt') -Encoding utf8 -Value 'The launcher puts your ISO, the game it builds, saves, logs and lcs.ini here.'
[IO.Compression.ZipFile]::CreateFromDirectory($folder, $zip, [IO.Compression.CompressionLevel]::Optimal, $true)
Get-Item -LiteralPath $zip

