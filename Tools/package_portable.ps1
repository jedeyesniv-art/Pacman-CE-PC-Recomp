# Builds the Release configuration and assembles a self-contained folder: dist\PacManCE\
#   pacman.exe + runtime DLLs + Visual C++ runtime DLLs + README + licences, and an empty game\ folder.
# Game files are only copied when -GameData is given (dist\ is git-ignored; never commit game files).
#
# One build for Windows and the Steam Deck (Proton). It is built against the SDK compiled from source with
# the Vulkan backend and our patches (tools\rexglue-sdk-vk, see CLAUDE.md), with line-table debug info so that
# crash logs can be symbolized: the matching PDBs go to dist\symbols\ (not part of the play folder).
#
# Usage (from anywhere):
#   powershell -ExecutionPolicy Bypass -File tools\package_portable.ps1 [-GameData C:\PacManRecomp\private] [-SkipBuild]
param(
    [string]$GameData = "",
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$project = Join-Path $repo "rexglue"
$buildDir = Join-Path $project "out\build\win-amd64-release"
$sdk = Join-Path $repo "tools\rexglue-sdk-vk\win-amd64"
$sdkPdbs = Join-Path $repo "tools\rexglue-sdk-src\out\win-amd64\Release"
$dist = Join-Path $repo "dist\PacManCE"
$symbols = Join-Path $repo "dist\symbols"
if (-not (Test-Path (Join-Path $sdk "bin\rexgpu-xenos.dll"))) {
    throw "SDK not found at $sdk - build it from source first (CLAUDE.md, 'Vulkan build')"
}

$vsRoot = "C:\Program Files\Microsoft Visual Studio\18\Community"
$vcvars = Join-Path $vsRoot "VC\Auxiliary\Build\vcvars64.bat"
$llvmBin = Join-Path $vsRoot "VC\Tools\Llvm\x64\bin"

if (-not $SkipBuild) {
    # vcvars64 needs vswhere on PATH; it reports VsDevCmd errors but the x64 environment still works,
    # so the commands are chained with "&" and success is judged by the build output instead.
    $env:PATH = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer;$env:PATH"
    $sdkCmake = $sdk -replace '\\', '/'
    $log = Join-Path $buildDir "..\package_build.log"
    New-Item -ItemType Directory -Force (Split-Path $log) | Out-Null
    $debugInfo = "`"-DCMAKE_CXX_FLAGS_RELEASE=-O3 -DNDEBUG -gline-tables-only -gcodeview`" `"-DCMAKE_C_FLAGS_RELEASE=-O3 -DNDEBUG -gline-tables-only -gcodeview`" `"-DCMAKE_EXE_LINKER_FLAGS_RELEASE=-Wl,-debug -Wl,-opt:ref -Wl,-opt:icf`""
    cmd /c "`"$vcvars`" >nul 2>&1 & set `"PATH=$llvmBin;%PATH%`" & cd /d `"$project`" & cmake --preset win-amd64-release -DCMAKE_PREFIX_PATH=$sdkCmake $debugInfo > `"$log`" 2>&1 & cmake --build `"$buildDir`" >> `"$log`" 2>&1"
    if (Select-String -Path $log -Pattern "FAILED|error:" -Quiet) {
        Get-Content $log | Select-String -Pattern "FAILED|error:" | Select-Object -First 20
        throw "Release build failed; see $log"
    }
    Write-Host "Release build OK"
}

# Visual C++ runtime DLLs the binaries import (msvcp140, msvcp140_atomic_wait, vcruntime140, vcruntime140_1).
$crt = Get-ChildItem (Join-Path $vsRoot "VC\Redist\MSVC") -Directory |
    Where-Object { $_.Name -match '^\d+\.\d+\.\d+$' } | Sort-Object { [version]$_.Name } -Descending |
    ForEach-Object { Get-ChildItem (Join-Path $_.FullName "x64") -Directory -Filter "Microsoft.VC*.CRT" } |
    Select-Object -First 1
if (-not $crt) { throw "Visual C++ redistributable folder not found under $vsRoot\VC\Redist\MSVC" }
$crtFiles = "msvcp140.dll", "msvcp140_atomic_wait.dll", "vcruntime140.dll", "vcruntime140_1.dll"

# Fresh folder, but keep game\ and userdata\ if they are already there.
New-Item -ItemType Directory -Force $dist | Out-Null
Get-ChildItem $dist -File | Remove-Item -Force
New-Item -ItemType Directory -Force (Join-Path $dist "game"), (Join-Path $dist "licenses") | Out-Null

# The SDK DLLs come straight from the SDK: the build folder only re-copies them when pacman.exe relinks.
Copy-Item (Join-Path $buildDir "pacman.exe"), (Join-Path $sdk "bin\rexruntime.dll"), (Join-Path $sdk "bin\rexgpu-xenos.dll") $dist
foreach ($f in $crtFiles) { Copy-Item (Join-Path $crt.FullName $f) $dist }
Copy-Item (Join-Path $project "README-portable.txt") (Join-Path $dist "README.txt")
# Double-click launcher for a frame-pacing trace session (frame trace + sampling profiler -> pacman-trace.log).
Copy-Item (Join-Path $project "trace-session.bat") $dist
# Double-click launcher: Championship mode with a random maze order (--pacman_random_mazes).
Copy-Item (Join-Path $project "random-mazes.bat") $dist
# Licences: our notices (rexglue\licenses: overview, ReXGlue, disruptorplus) plus the licence texts of every
# third-party library linked into the binaries, from the SDK source tree they were built from. Keep this list in
# step with rexglue\licenses\THIRD-PARTY-NOTICES.txt.
Get-ChildItem (Join-Path $dist "licenses") -File | Remove-Item -Force
Copy-Item (Join-Path $project "licenses\*") (Join-Path $dist "licenses")
Copy-Item (Join-Path $repo "LICENSE") (Join-Path $dist "licenses\PacManCE-LICENSE.txt")
$thirdparty = Join-Path $repo "tools\rexglue-sdk-src\thirdparty"
$licenseFiles = [ordered]@{
    "sdl3\LICENSE.txt"                            = "SDL3-LICENSE.txt"
    "FFmpeg\COPYING.LGPLv2.1"                     = "LGPL-2.1.txt"
    "FFmpeg\LICENSE.md"                           = "FFmpeg-LICENSE.md"
    "libmspack\libmspack\COPYING.LIB"             = "libmspack-COPYING.LIB.txt"
    "glslang\LICENSE.txt"                         = "glslang-LICENSE.txt"
    "vulkan-headers\LICENSE.md"                   = "Vulkan-Headers-LICENSE.md"
    "vulkan-headers\LICENSES\MIT.txt"             = "Vulkan-Headers-MIT.txt"
    "spirv-headers\LICENSE"                       = "SPIRV-Headers-LICENSE.txt"
    "vulkan-memory-allocator\LICENSE.txt"         = "VulkanMemoryAllocator-LICENSE.txt"
    "imgui\LICENSE.txt"                           = "imgui-LICENSE.txt"
    "fmt\LICENSE"                                 = "fmt-LICENSE.txt"
    "spdlog\LICENSE"                              = "spdlog-LICENSE.txt"
    "tomlplusplus\LICENSE"                        = "tomlplusplus-LICENSE.txt"
    "simde\COPYING"                               = "simde-LICENSE.txt"
    "xxHash\LICENSE"                              = "xxHash-LICENSE.txt"
    "utfcpp\LICENSE"                              = "utfcpp-LICENSE.txt"
    "o1heap\LICENSE"                              = "o1heap-LICENSE.txt"
    "aes_128\LICENSE"                             = "aes_128-LICENSE.txt"
}
foreach ($entry in $licenseFiles.GetEnumerator()) {
    $source = Join-Path $thirdparty $entry.Key
    if (-not (Test-Path $source)) { throw "Licence file missing: $source" }
    Copy-Item $source (Join-Path $dist "licenses\$($entry.Value)")
}

if ($GameData) {
    if (-not (Test-Path (Join-Path $GameData "default.xex"))) { throw "$GameData has no default.xex" }
    # Everything except default.exe (the decrypted basefile made for analysis, not a game file). The XBLA
    # metadata (ArcadeInfo.xml, achievement and rating images) is copied too, in case the runtime uses it.
    Get-ChildItem $GameData | Where-Object { $_.Name -ne "default.exe" } |
        Copy-Item -Destination (Join-Path $dist "game") -Recurse -Force
    Write-Host "Copied game files from $GameData"
}

# Symbols for exactly these binaries, to read crash logs and profiles (llvm-symbolizer --obj=<binary> <address>).
if (Test-Path $symbols) { Remove-Item $symbols -Recurse -Force }
New-Item -ItemType Directory -Force $symbols | Out-Null
Copy-Item (Join-Path $dist "pacman.exe"), (Join-Path $buildDir "pacman.pdb"), (Join-Path $dist "rexruntime.dll"),
    (Join-Path $dist "rexgpu-xenos.dll"), (Join-Path $sdkPdbs "rexruntime.pdb"), (Join-Path $sdkPdbs "rexgpu-xenos.pdb") $symbols

Write-Host "Portable folder: $dist"
Get-ChildItem $dist | Select-Object Name, Length | Format-Table -AutoSize
