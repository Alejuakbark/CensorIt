# Build obs_license_plate_blur.dll (Windows).
# Linux / macOS: use ./build.sh in this folder (same options conceptually).
#
# You always need libobs headers + obs.lib from the SAME OBS major.minor you run
# (build OBS from source, or use your existing OBS developer/build tree).
#
# OpenCV (DNN):
#   A) Set CMAKE_PREFIX_PATH to a normal OpenCV install that contains OpenCVConfig.cmake, or
#   B) Omit CMAKE_PREFIX_PATH and pass -UseVcpkg (uses VCPKG_ROOT or clones tools\vcpkg).
#
# Examples:
#   .\build.ps1 -UseVcpkg `
#     -LibObsInclude "D:\obs-studio\build\libobs" `
#     -LibObsLib "D:\obs-studio\build\libobs\Release\obs.lib"
#
#   $env:CMAKE_PREFIX_PATH = "C:\opencv\build"
#   .\build.ps1 -LibObsInclude "..." -LibObsLib "..."
#
# After a dynamic OpenCV (default vcpkg triplet), copy the OpenCV DLLs next to obs64.exe or
# onto PATH if the plugin fails to load.

[CmdletBinding()]
param(
  [string] $OpenCV = $env:CMAKE_PREFIX_PATH,
  [string] $LibObsInclude = $env:LIBOBS_INCLUDE_DIR,
  [string] $LibObsLib = $env:LIBOBS_LIB,
  [string] $VcpkgRoot = $env:VCPKG_ROOT,
  [switch] $UseVcpkg
)

$ErrorActionPreference = "Stop"

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $here

function Test-OpenCvPrefix([string] $p) {
  if (-not $p) { return $false }
  return (Test-Path (Join-Path $p "OpenCVConfig.cmake")) -or (Test-Path (Join-Path $p "share\OpenCV\OpenCVConfig.cmake"))
}

function Get-VcpkgToolchainPath([string] $root) {
  if (-not $root) { return $null }
  $tc = Join-Path $root "scripts\buildsystems\vcpkg.cmake"
  if (Test-Path $tc) { return $tc }
  return $null
}

if (-not $LibObsInclude) { throw "Set -LibObsInclude or `$env:LIBOBS_INCLUDE_DIR to the folder containing obs.h (usually ...\libobs)." }
if (-not (Test-Path $LibObsInclude)) { throw "LIBOBS include dir not found: $LibObsInclude" }
if (-not $LibObsLib) { throw "Set -LibObsLib or `$env:LIBOBS_LIB to the full path to obs.lib." }
if (-not (Test-Path $LibObsLib)) { throw "obs.lib not found: $LibObsLib" }

$toolchain = $null
$vcpkgInstalled = Join-Path $here "vcpkg_installed\x64-windows"

if (-not (Test-OpenCvPrefix $OpenCV)) {
  $OpenCV = $null
}

if (-not $OpenCV -and (Test-OpenCvPrefix $vcpkgInstalled)) {
  $OpenCV = $vcpkgInstalled
}

if ($UseVcpkg) {
  if (-not $VcpkgRoot) {
    $localVcpkg = Join-Path $here "tools\vcpkg"
    if (Test-Path (Join-Path $localVcpkg "vcpkg.exe")) {
      $VcpkgRoot = $localVcpkg
    }
  }

  if (-not $VcpkgRoot) {
    $localVcpkg = Join-Path $here "tools\vcpkg"
    if (-not (Test-Path (Join-Path $localVcpkg "vcpkg.exe"))) {
      $git = Get-Command git -ErrorAction SilentlyContinue
      if (-not $git) {
        throw "Git is required to clone vcpkg. Install Git for Windows, then re-run with -UseVcpkg, or set VCPKG_ROOT to an existing vcpkg clone."
      }
      New-Item -ItemType Directory -Force -Path (Split-Path $localVcpkg) | Out-Null
      Write-Host "Cloning vcpkg into $localVcpkg (one-time)..."
      git clone https://github.com/microsoft/vcpkg.git $localVcpkg --depth 1
      Write-Host "Bootstrapping vcpkg..."
      & (Join-Path $localVcpkg "bootstrap-vcpkg.bat") -disableMetrics
    }
    $VcpkgRoot = $localVcpkg
  }

  $toolchain = Get-VcpkgToolchainPath $VcpkgRoot
  if (-not $toolchain) {
    throw "vcpkg toolchain file not found under VCPKG_ROOT=$VcpkgRoot"
  }
} elseif (-not $OpenCV) {
  throw @"
OpenCV not found. Either:
  - Install OpenCV with DNN and set `$env:CMAKE_PREFIX_PATH to its build folder (contains OpenCVConfig.cmake), or
  - Run with -UseVcpkg so OpenCV is built via vcpkg (first run can take a long time). Example:
      .\build.ps1 -UseVcpkg -LibObsInclude 'D:\obs\libobs' -LibObsLib 'D:\obs\build\libobs\Release\obs.lib'
"@
}

$cmakeArgs = @(
  "-B", "build",
  "-A", "x64",
  "-D", "LIBOBS_INCLUDE_DIR=$LibObsInclude",
  "-D", "LIBOBS_LIB=$LibObsLib"
)

if ($toolchain) {
  Write-Host "Using vcpkg toolchain: $toolchain (OpenCV from vcpkg.json manifest)"
  $cmakeArgs += "-DCMAKE_TOOLCHAIN_FILE=$toolchain"
  $cmakeArgs += "-DVCPKG_TARGET_TRIPLET=x64-windows"
} elseif (Test-OpenCvPrefix $OpenCV) {
  Write-Host "Using OpenCV from: $OpenCV"
  $cmakeArgs += "-DCMAKE_PREFIX_PATH=$OpenCV"
} else {
  throw "OpenCV prefix resolution failed (unexpected)."
}

$cmakeExe = Get-Command cmake -ErrorAction SilentlyContinue
if (-not $cmakeExe) {
  throw "cmake not found on PATH. Install CMake (e.g. winget install Kitware.CMake) and reopen the terminal."
}

Write-Host "Configuring..."
& cmake @cmakeArgs

Write-Host "Building Release..."
& cmake --build build --config Release

Write-Host ""
Write-Host "Done. Output (typical): $here\build\Release\obs_license_plate_blur.dll"
Write-Host "Install into OBS (DLL + data; use Admin if OBS is under Program Files):"
Write-Host "  powershell -ExecutionPolicy Bypass -File `"$here\scripts\install-to-obs.ps1`""
