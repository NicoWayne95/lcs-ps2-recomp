param(
    [Parameter(Mandatory)][string]$Source,
    [Parameter(Mandatory)][string]$Destination,
    [Parameter(Mandatory)][string]$Executable,
    [Parameter(Mandatory)][string]$ProcessName
)
$ErrorActionPreference = 'Stop'
if (Get-Process -Name $ProcessName -ErrorAction SilentlyContinue) { throw "$ProcessName läuft. Bitte vor der Übernahme schließen." }
if (!(Test-Path -LiteralPath (Join-Path $Source $Executable) -PathType Leaf)) { throw 'Build-EXE fehlt.' }
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$work = Join-Path $root ('backups/builds/' + [guid]::NewGuid().ToString('N'))
$incoming = Join-Path $work 'incoming'
$previous = Join-Path $work 'previous'
New-Item -ItemType Directory -Path $incoming,$previous | Out-Null
$files = if ($ProcessName -eq 'LCSrecomp') {
    Get-ChildItem -LiteralPath $Source -File | Where-Object { $_.Extension -in '.dll','.pdb' -or $_.Name -eq $Executable }
} else { Get-Item -LiteralPath (Join-Path $Source $Executable) }
foreach ($file in $files) { Copy-Item -LiteralPath $file.FullName -Destination $incoming }
$replaced = [Collections.Generic.List[string]]::new()
$added = [Collections.Generic.List[string]]::new()
try {
    foreach ($file in ($files | Sort-Object { $_.Name -eq $Executable })) {
        $target = Join-Path $Destination $file.Name
        $next = Join-Path $incoming $file.Name
        if (Test-Path -LiteralPath $target -PathType Leaf) {
            [IO.File]::Replace($next,$target,(Join-Path $previous $file.Name))
            $replaced.Add($file.Name)
        } else {
            [IO.File]::Move($next,$target)
            $added.Add($file.Name)
        }
    }
} catch {
    foreach ($name in $replaced) {
        [IO.File]::Replace((Join-Path $previous $name),(Join-Path $Destination $name),(Join-Path $incoming $name))
    }
    foreach ($name in $added) { [IO.File]::Move((Join-Path $Destination $name),(Join-Path $incoming $name)) }
    throw
}
Get-Item -LiteralPath (Join-Path $Destination $Executable)
