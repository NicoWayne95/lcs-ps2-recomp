param(
    [Parameter(Mandatory)][string]$PortableZip,
    [string]$Iso
)
$ErrorActionPreference = 'Stop'
$work = Join-Path $PSScriptRoot ('tests/portable/' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $work | Out-Null
Expand-Archive -LiteralPath $PortableZip -DestinationPath $work
$app = Join-Path $work 'LCS Recomp'
$exe = Join-Path $app 'lcs-recomp-launcher.exe'
$kit = Join-Path $app 'Source Code'
if (!(Test-Path -LiteralPath $exe -PathType Leaf)) { throw 'Launcher fehlt.' }
$manifest = Get-Content -Raw -LiteralPath (Join-Path $kit 'kit.json') | ConvertFrom-Json
$entries = @(Get-ChildItem -LiteralPath $kit -Force -Recurse -File)
if ($entries.Count -ne (1 + $manifest.file_count) -or $manifest.files.Count -ne $manifest.file_count) { throw 'Kit-Dateizahl weicht ab.' }
$seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($entry in $manifest.files) {
    if ($entry.path -match '(^|/)\.\.?(/|$)|[:\\]' -or $entry.path.StartsWith('/') -or !$seen.Add($entry.path)) { throw "Ungültiger Kit-Pfad: $($entry.path)" }
    $file = Get-Item -LiteralPath (Join-Path $kit $entry.path)
    if ($file.Length -ne $entry.size -or (Get-FileHash -LiteralPath $file.FullName).Hash -ne $entry.sha256) { throw "Kit-Datei weicht ab: $($entry.path)" }
}
if ($Iso) {
    $isoCopy = Join-Path $work 'test-input.iso'
    Copy-Item -LiteralPath $Iso -Destination $isoCopy
    $game = Join-Path $app 'game'
    $tools = (Get-Content -Raw -LiteralPath (Join-Path $kit 'config/tools.json') | ConvertFrom-Json).tools
    $toolchain = Join-Path $game '_build/toolchain'
    New-Item -ItemType Directory -Path $toolchain | Out-Null
    foreach ($tool in $tools) {
        $name = [IO.Path]::GetFileName(([uri]$tool.url).AbsolutePath)
        $archive = Join-Path $PSScriptRoot "tools/toolchains/archives/$name"
        if (Test-Path -LiteralPath $archive) {
            if ((Get-Item -LiteralPath $archive).Length -ne $tool.size -or (Get-FileHash -LiteralPath $archive).Hash -ne $tool.sha256) {
                throw "Toolchain-Archiv weicht vom Kit-Pin ab: $name"
            }
            Copy-Item -LiteralPath $archive -Destination (Join-Path $toolchain "$($tool.folder).zip")
        }
    }
    $proc = Start-Process -Wait -PassThru -FilePath $exe -WorkingDirectory $app -ArgumentList ('--prepare "{0}" "{1}"' -f $isoCopy,$game)
    if ($proc.ExitCode -ne 0 -or !(Test-Path -LiteralPath (Join-Path $game 'LCSrecomp.exe'))) { throw "Lokaler Spielbuild fehlgeschlagen. Testordner: $work" }
}
Get-Item -LiteralPath $work

