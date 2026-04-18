# Download the bundled default license-plate ONNX (YOLOv8-style) into the plugin data tree.
# Source: Hugging Face model card "ml-debi/yolov8-license-plate-detection" (best.onnx).
# Use only where the model license allows; verify terms on the model page before redistribution.

$ErrorActionPreference = "Stop"
$repo = Resolve-Path (Join-Path $PSScriptRoot "..")
$url = "https://huggingface.co/ml-debi/yolov8-license-plate-detection/resolve/main/best.onnx"
$destDir = Join-Path $repo "data\obs-plugins\obs_license_plate_blur\models"
$dest = Join-Path $destDir "license_plate.onnx"

New-Item -ItemType Directory -Force -Path $destDir | Out-Null
Write-Host "Downloading model to $dest ..."
Invoke-WebRequest -Uri $url -OutFile $dest -MaximumRedirection 5 -UseBasicParsing
Write-Host "Done ($((Get-Item $dest).Length) bytes)."
