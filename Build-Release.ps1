param(
    [Parameter(Mandatory)][string]$Checkpoint,
    [switch]$BuildLauncher,
    [switch]$UseInstalledDependencies,
    [switch]$Sign,
    [switch]$Installer,
    [string]$IsccPath,
    [string]$MinLauncher = '0.2.0'
)
$ErrorActionPreference = 'Stop'
if ([string]::IsNullOrWhiteSpace($Checkpoint)) { throw 'Quellcode-Checkpoint angeben.' }
$root = $PSScriptRoot
$game = Join-Path $root 'game'
$launcher = Join-Path $root 'launcher'
if ($BuildLauncher) { & (Join-Path $root 'Build-Launcher.ps1') -UseInstalledDependencies:$UseInstalledDependencies }
$exe = if ($BuildLauncher) { Join-Path $root 'build/launcher/release/lcs-recomp-launcher.exe' } else { Join-Path $root 'bin/launcher/lcs-recomp-launcher.exe' }
$version = (Get-Content (Join-Path $game 'VERSION') -Raw).Trim()
$stage = Join-Path $root ('releases/staging/{0}-{1}' -f $version, [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $stage | Out-Null
$kit = Join-Path $stage 'Source Code'
& python.exe (Join-Path $game 'tools/make_kit.py') $kit (Join-Path $root 'backups/reference/recompiler-history') (Join-Path $game 'config/lcs_guest.toml') (Join-Path $game 'tools/lcs_vu1_uploads.txt')
if ($LASTEXITCODE -ne 0) { throw 'Kit-Erstellung fehlgeschlagen.' }
$launcherVersion = (Get-Content (Join-Path $launcher 'src-tauri/tauri.conf.json') -Raw | ConvertFrom-Json).version
& (Join-Path $launcher 'installer/make_portable_zip.ps1') -Version $launcherVersion -LauncherExe $exe -KitPath $kit -OutputDirectory $stage
if ($Installer) {
    if (!$IsccPath) {
        $IsccPath = @(
            (Get-Command ISCC.exe -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source)
            (Join-Path $env:LOCALAPPDATA 'Programs/Inno Setup 6/ISCC.exe')
            (Join-Path $env:USERPROFILE 'AppData/Local/Programs/Inno Setup 6/ISCC.exe')
        ) | Where-Object { $_ -and (Test-Path -LiteralPath $_ -PathType Leaf) } | Select-Object -First 1
    }
    if (!$IsccPath) { throw 'ISCC.exe fehlt. -IsccPath angeben.' }
    & $IsccPath /Q "/DLauncherSource=$exe" "/DKitSource=$kit" "/O$stage" (Join-Path $launcher 'installer/lcs_recomp_launcher.iss')
    if ($LASTEXITCODE -ne 0) { throw 'Inno Setup fehlgeschlagen.' }
}
if ($Sign) {
    $tool = Join-Path $root 'build/release-tool'
    & cargo.exe build --locked --release --manifest-path (Join-Path $root 'tools/release-tool/Cargo.toml') --target-dir $tool
    if ($LASTEXITCODE -ne 0) { throw 'Release-Tool-Build fehlgeschlagen.' }
    $release = Join-Path $tool 'release/lcs-release.exe'
    $manifestDir = Join-Path $stage 'signed'
    $args = @('make',$kit,$manifestDir,'--min-launcher',$MinLauncher)
    if ($Installer) {
        $setup = Get-ChildItem -LiteralPath $stage -Filter '*Setup-*.exe' | Select-Object -First 1
        if (!$setup) { throw 'Installer für das Manifest fehlt.' }
        $args += @('--setup',$setup.FullName,'--launcher-version',$launcherVersion)
    }
    & $release @args
    if ($LASTEXITCODE -ne 0) { throw 'Manifest-Erstellung fehlgeschlagen.' }
    $source = Get-Content (Join-Path $launcher 'src-tauri/src/update.rs') -Raw
    $public = [regex]::Match($source,'PUBLIC_KEY_HEX: &str = "([0-9a-f]{64})"').Groups[1].Value
    if (!$public) { throw 'Öffentlicher Launcher-Schlüssel fehlt.' }
    & $release verify $manifestDir --pub $public
    if ($LASTEXITCODE -ne 0) { throw 'Manifestprüfung fehlgeschlagen.' }
}
$metadata = [ordered]@{
    checkpoint = $Checkpoint
    runtime = $version
    launcher = $launcherVersion
    launcher_sha256 = (Get-FileHash -LiteralPath $exe).Hash
    kit_manifest_sha256 = (Get-FileHash -LiteralPath (Join-Path $kit 'kit.json')).Hash
    configuration = 'Release / Ninja / D3D11'
    signed = [bool]$Sign
}
$metadata | ConvertTo-Json | Set-Content -Encoding utf8 -LiteralPath (Join-Path $stage 'build-info.json')
Get-ChildItem -LiteralPath $stage -Recurse -File -Include '*.zip','*.exe','manifest.json','manifest.json.sig' |
    Get-FileHash | ForEach-Object { '{0}  {1}' -f $_.Hash, [IO.Path]::GetRelativePath($stage,$_.Path) } |
    Set-Content -Encoding utf8 -LiteralPath (Join-Path $stage 'SHA256SUMS.txt')
Get-Item -LiteralPath $stage

