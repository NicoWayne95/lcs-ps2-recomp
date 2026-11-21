param([switch]$TraceRadar)

$ErrorActionPreference = 'Stop'
$dir = Join-Path $PSScriptRoot 'bin/current'
$launch = @{
    FilePath = Join-Path $dir 'LCSrecomp.exe'
    WorkingDirectory = $dir
}
if ($TraceRadar) {
    $launch.Environment = @{ LCS_TRACE_RADAR_GS = '1' }
}
Start-Process @launch
