# Builds the Release configuration and assembles a self-contained folder: dist\PacManCE\
#   pacman.exe + runtime DLLs + Visual C++ runtime DLLs + README + licences, and an empty game\ folder.
# Game files are only copied when -GameData is given (dist\ is git-ignored; never commit game files).
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
$sdk = Join-Path $repo "tools\rexglue-sdk\win-amd64"
$dist = Join-Path $repo "dist\PacManCE"

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
    cmd /c "`"$vcvars`" >nul 2>&1 & set `"PATH=$llvmBin;%PATH%`" & cd /d `"$project`" & cmake --preset win-amd64-release -DCMAKE_PREFIX_PATH=$sdkCmake > `"$log`" 2>&1 & cmake --build --preset win-amd64-release >> `"$log`" 2>&1"
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

Copy-Item (Join-Path $buildDir "pacman.exe"), (Join-Path $buildDir "rexruntime.dll"), (Join-Path $buildDir "rexgpu-xenos.dll") $dist
foreach ($f in $crtFiles) { Copy-Item (Join-Path $crt.FullName $f) $dist }
Copy-Item (Join-Path $project "README-portable.txt") (Join-Path $dist "README.txt")
Copy-Item (Join-Path $project "licenses\*") (Join-Path $dist "licenses")
Copy-Item (Join-Path $sdk "licenses\SDL3\LICENSE.txt") (Join-Path $dist "licenses\SDL3-LICENSE.txt")

if ($GameData) {
    if (-not (Test-Path (Join-Path $GameData "default.xex"))) { throw "$GameData has no default.xex" }
    # Everything except default.exe (the decrypted basefile made for analysis, not a game file). The XBLA
    # metadata (ArcadeInfo.xml, achievement and rating images) is copied too, in case the runtime uses it.
    Get-ChildItem $GameData | Where-Object { $_.Name -ne "default.exe" } |
        Copy-Item -Destination (Join-Path $dist "game") -Recurse -Force
    Write-Host "Copied game files from $GameData"
}

Write-Host "Portable folder: $dist"
Get-ChildItem $dist | Select-Object Name, Length | Format-Table -AutoSize
