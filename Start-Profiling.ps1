param(
    [int]$SpikeMs,
    [switch]$NativeAudit,
    [switch]$GpuAudit,
    [switch]$TracePlayerPush,
    [switch]$TraceTyreSmoke,
    [switch]$DirectItemsOff,
    [ValidateSet('Default','Tearing','VSync','Immediate')][string]$PresentMode = 'Default',
    [ValidateRange(24,360)][int]$HostHz = 60,
    [ValidateSet('Current','PipelineOff','PipelineOn')][string]$Variant = 'Current'
)
$ErrorActionPreference = 'Stop'
if (Get-Process LCSrecomp -ErrorAction SilentlyContinue) { throw 'LCSrecomp is already running.' }
if ($Variant -eq 'Current' -and ($PresentMode -ne 'Default' -or $HostHz -ne 60 -or $DirectItemsOff -or $TracePlayerPush -or $TraceTyreSmoke)) {
    throw 'The selected options require the performance test build.'
}
$dir = Join-Path $PSScriptRoot 'bin/current'
$exe = if ($Variant -eq 'Current') { Join-Path $dir 'LCSrecomp.exe' } else {
    Join-Path $PSScriptRoot 'build/game/out/LCSrecomp.exe'
}
if (!(Test-Path -LiteralPath $exe -PathType Leaf)) { throw "Build missing: $exe" }
$out = Join-Path $PSScriptRoot ('benchmarks/profiles/' + $Variant + '-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $out | Out-Null
$environment = @{
    LCS_TIMING_CSV = Join-Path $out 'timing.csv'
    LCS_FLIP_CSV = Join-Path $out 'flips.csv'
    LCS_PRESENT_CSV = Join-Path $out 'presents.csv'
    LCS_HOST_HZ = [string]$HostHz
}
if ($PresentMode -ne 'Default') { $environment.LCS_PROFILE_PRESENT = $PresentMode.ToLowerInvariant() }
if ($Variant -ne 'Current') {
    $environment.LCS_D3D11_REUSE_PIPELINE = if ($Variant -eq 'PipelineOn') { '1' } else { '0' }
    $environment.LCS_RSL_DIRECT_ITEMS = if ($DirectItemsOff) { '0' } else { '1' }
}
if ($NativeAudit -or $GpuAudit -or $SpikeMs -gt 0) { $environment.PS2X_PERF_LOG = Join-Path $out 'phases.csv' }
if ($TracePlayerPush) { $environment.LCS_TRACE_PLAYER_PUSH = '1' }
if ($TraceTyreSmoke) { $environment.LCS_TRACE_TYRE_SMOKE = '1' }
if ($GpuAudit) { $environment.PS2X_GPU_TIMING = '1' }
if ($NativeAudit) {
    $environment.PS2X_HLE_STATS = '1'
    $environment.LCS_RSL_WORLD_LOG = '1'
    $environment.PS2X_WRITE_WATCH_LOG = '1'
}
if ($SpikeMs -gt 0) {
    $environment.PS2X_HOST_SPIKES = [string]$SpikeMs
    $environment.LCS_PROFILE = '10'
    $environment.LCS_PROFILE_THREAD = 'GameThread,RenderWorker,GsStage,MainThread'
    $environment.LCS_PROFILE_GUEST_FILE = Join-Path $out 'guest_profile.txt'
    $environment.LCS_PROFILE_SPIKES = [string]$SpikeMs
}
@{
    Variant = $Variant
    Executable = $exe
    Sha256 = (Get-FileHash -LiteralPath $exe).Hash
    SpikeMs = $SpikeMs
    HostHz = $HostHz
    PresentMode = $PresentMode
    NativeAudit = [bool]$NativeAudit
    GpuAudit = [bool]$GpuAudit
    TracePlayerPush = [bool]$TracePlayerPush
    TraceTyreSmoke = [bool]$TraceTyreSmoke
    DirectItemsOff = [bool]$DirectItemsOff
} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $out 'run.json') -Encoding utf8
$start = New-Object System.Diagnostics.ProcessStartInfo
$start.FileName = $exe
$start.Arguments = '--game-dir "{0}"' -f $dir
$start.WorkingDirectory = $dir
$start.UseShellExecute = $false
foreach ($key in $environment.Keys) {
    $start.EnvironmentVariables[$key] = [string]$environment[$key]
}
[System.Diagnostics.Process]::Start($start).Dispose()
Get-Item -LiteralPath $out
