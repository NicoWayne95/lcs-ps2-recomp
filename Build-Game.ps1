param([switch]$ConfigureOnly, [switch]$Fresh, [switch]$Install, [ValidateSet('AVX','AVX2','SSE4')][string]$Cpu = 'AVX')
$ErrorActionPreference = 'Stop'
if (Get-Process LCSrecomp -ErrorAction SilentlyContinue) { throw 'LCSrecomp läuft. Bitte vor dem Build schließen.' }
$source = (Join-Path $PSScriptRoot 'game').Replace('\','/')
$build = (Join-Path $PSScriptRoot 'build/game').Replace('\','/')
$out = (Join-Path $build 'out').Replace('\','/')
$deps = (Join-Path $source 'deps').Replace('\','/')
$llvm = (Join-Path $PSScriptRoot 'tools/toolchains/llvm-mingw-20261020-ucrt-x86_64/bin').Replace('\','/')
$ninja = (Get-Command ninja.exe).Source
$python = (Get-Command python.exe).Source
$env:Path = "$llvm;$env:Path"
$args = @('-S',$source,'-B',$build,'-G','Ninja','-DCMAKE_BUILD_TYPE=Release',
    '-DLCS_DIAGNOSTICS=OFF','-DLCS_MENU_VIDEO=ON','-DLCS_RUNTIME_DEBUG_INFO=ON','-DLCS_RHI_VULKAN=OFF',
    '-DLCS_BUILD_TOOLS=OFF',"-DPS2X_CPU=$Cpu",'-DPS2X_LTO=OFF','-DPS2X_ENABLE_AGRESSIVE_LOGS=OFF',
    '-DCMAKE_EXE_LINKER_FLAGS=-static','-DFETCHCONTENT_FULLY_DISCONNECTED=ON',
    "-DCMAKE_C_COMPILER=$llvm/clang.exe","-DCMAKE_CXX_COMPILER=$llvm/clang++.exe",
    "-DCMAKE_RC_COMPILER=$llvm/llvm-windres.exe","-DCMAKE_MAKE_PROGRAM=$ninja","-DPython3_EXECUTABLE=$python",
    "-DLCS_GAME_DIR=$out","-DFFMPEG_PREBUILT_URL=$deps/ffmpeg-n7.1-241205-lgpl-amd64-shared.zip")
foreach ($name in @('elfio','fmt','imgui','libdwarf','nlohmann_json','rabbitizer','SDL3','toml11','zlib')) {
    $args += "-DFETCHCONTENT_SOURCE_DIR_$($name.ToUpperInvariant())=$deps/$name"
}
if ($Fresh) { $args = @('--fresh') + $args }
& cmake.exe @args
if ($LASTEXITCODE -ne 0) { throw 'CMake-Konfiguration fehlgeschlagen.' }
if ($ConfigureOnly) { return }
& cmake.exe --build $build
if ($LASTEXITCODE -ne 0) { throw 'Spielbuild fehlgeschlagen.' }
if ($Install) {
    & (Join-Path $PSScriptRoot 'tools/scripts/Install-Build.ps1') -Source $out -Destination (Join-Path $PSScriptRoot 'bin/current') -Executable 'LCSrecomp.exe' -ProcessName 'LCSrecomp'
} else { Get-Item -LiteralPath "$out/LCSrecomp.exe" }

