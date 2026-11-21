$ErrorActionPreference = 'Stop'
$dir = Join-Path $PSScriptRoot 'bin/launcher'
$game = Join-Path $PSScriptRoot 'bin/current'
Start-Process -FilePath (Join-Path $dir 'lcs-recomp-launcher.exe') -WorkingDirectory $dir -ArgumentList ('--game-dir "{0}"' -f $game)

