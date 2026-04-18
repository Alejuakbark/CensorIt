# Build obs_license_plate_blur.dll (requires OpenCV + libobs for your OBS version).
# Example:
#   $env:CMAKE_PREFIX_PATH = "C:\opencv\build"
#   $env:LIBOBS_INCLUDE_DIR = "C:\obs-build\libobs"
#   $env:LIBOBS_LIB = "C:\obs-build\libobs\Release\obs.lib"
#   .\build.ps1

$ErrorActionPreference = "Stop"

$opencv = $env:CMAKE_PREFIX_PATH
$include = $env:LIBOBS_INCLUDE_DIR
$lib = $env:LIBOBS_LIB

if (-not $opencv) { throw "Set CMAKE_PREFIX_PATH to your OpenCV build directory (contains OpenCVConfig.cmake)." }
if (-not $include) { throw "Set LIBOBS_INCLUDE_DIR to the folder containing obs.h." }
if (-not $lib) { throw "Set LIBOBS_LIB to the full path to obs.lib." }

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $here

cmake -B build -A x64 `
  -D CMAKE_PREFIX_PATH="$opencv" `
  -D LIBOBS_INCLUDE_DIR="$include" `
  -D LIBOBS_LIB="$lib"

cmake --build build --config Release

Write-Host "Output (typical): $here\build\Release\obs_license_plate_blur.dll"
