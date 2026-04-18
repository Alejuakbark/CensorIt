# Copy built plugin DLL + data (locale, models, etc.) into an OBS Studio install.
# Requires PowerShell run as Administrator if OBS is under Program Files.
#
# Example:
#   .\scripts\install-to-obs.ps1
#   .\scripts\install-to-obs.ps1 -ObsRoot "D:\Portable\obs-studio" -Dll "D:\...\build\Release\obs_license_plate_blur.dll"

[CmdletBinding()]
param(
  [string] $ObsRoot = "C:\Program Files\obs-studio",
  [string] $Dll = "",
  [switch] $DataOnly
)

$ErrorActionPreference = "Stop"
$repo = Resolve-Path (Join-Path $PSScriptRoot "..")

if (-not $Dll) {
  $Dll = Join-Path $repo "build\Release\obs_license_plate_blur.dll"
}

$plugDir = Join-Path $ObsRoot "obs-plugins\64bit"
$dataSrc = Join-Path $repo "data\obs-plugins\obs_license_plate_blur"
$dataDst = Join-Path $ObsRoot "data\obs-plugins\obs_license_plate_blur"

if (-not (Test-Path (Join-Path $ObsRoot "bin\64bit\obs64.exe"))) {
  Write-Warning "obs64.exe not found under $ObsRoot; check -ObsRoot."
}

Write-Host "Installing data to $dataDst ..."
New-Item -ItemType Directory -Force -Path $dataDst | Out-Null
Copy-Item -Path (Join-Path $dataSrc "*") -Destination $dataDst -Recurse -Force

if (-not $DataOnly) {
  if (-not (Test-Path $Dll)) {
    throw "DLL not found: $Dll. Build the project first, or pass -Dll."
  }
  if (-not (Test-Path $plugDir)) {
    throw "Plugin dir not found: $plugDir. Fix -ObsRoot."
  }
  Write-Host "Installing $Dll -> $plugDir ..."
  Copy-Item -Path $Dll -Destination $plugDir -Force
}

Write-Host "Done. Restart OBS if it was running."
