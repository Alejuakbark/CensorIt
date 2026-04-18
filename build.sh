#!/usr/bin/env bash
# Minimal Unix build (Linux / macOS) for obs_license_plate_blur — mirrors build.ps1.
#
# Requires: cmake, a C++17 compiler, libobs headers + libobs.{so,dylib}, OpenCV+DNN
#   (system install via CMAKE_PREFIX_PATH, or --use-vcpkg using VCPKG_ROOT or ./tools/vcpkg).
#
# Examples:
#   ./build.sh --use-vcpkg \
#     --libobs-include "$HOME/obs/build/libobs" \
#     --libobs-lib "$HOME/obs/build/libobs/libobs.so"
#
#   export CMAKE_PREFIX_PATH=/usr/local/opencv4
#   ./build.sh --libobs-include ... --libobs-lib ...
#
# Output: build/obs_license_plate_blur.so (Linux) or build/obs_license_plate_blur.dylib (macOS)

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"

usage() {
  echo "build.sh — configure & build obs_license_plate_blur for Linux/macOS."
  echo ""
  echo "Options:"
  echo "  --use-vcpkg              Build OpenCV from vcpkg.json (clone tools/vcpkg if needed)"
  echo "  --opencv PATH            OpenCV prefix (same as CMAKE_PREFIX_PATH)"
  echo "  --libobs-include PATH    Directory containing obs.h"
  echo "  --libobs-lib PATH        Full path to libobs.so or libobs.dylib"
  echo "  --vcpkg-root PATH        vcpkg clone (default: \$VCPKG_ROOT or ./tools/vcpkg)"
  echo "Env: CMAKE_PREFIX_PATH, LIBOBS_INCLUDE_DIR, LIBOBS_LIB, VCPKG_ROOT, VCPKG_TARGET_TRIPLET"
  exit "${1:-0}"
}

USE_VCPKG=0
CMAKE_PREFIX_PATH="${CMAKE_PREFIX_PATH:-}"
LIBOBS_INCLUDE_DIR="${LIBOBS_INCLUDE_DIR:-}"
LIBOBS_LIB="${LIBOBS_LIB:-}"
VCPKG_ROOT="${VCPKG_ROOT:-}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --use-vcpkg) USE_VCPKG=1; shift ;;
    --opencv) CMAKE_PREFIX_PATH="$2"; shift 2 ;;
    --libobs-include) LIBOBS_INCLUDE_DIR="$2"; shift 2 ;;
    --libobs-lib) LIBOBS_LIB="$2"; shift 2 ;;
    --vcpkg-root) VCPKG_ROOT="$2"; shift 2 ;;
    -h|--help) usage 0 ;;
    *) echo "Unknown option: $1" >&2; usage 1 ;;
  esac
done

opencv_ok() {
  [[ -n "$1" ]] && {
    [[ -f "$1/OpenCVConfig.cmake" ]] || [[ -f "$1/share/OpenCV/OpenCVConfig.cmake" ]]
  }
}

default_vcpkg_triplet() {
  if [[ -n "${VCPKG_TARGET_TRIPLET:-}" ]]; then
    echo "$VCPKG_TARGET_TRIPLET"
    return
  fi
  case "$(uname -s)" in
    Linux)
      case "$(uname -m)" in
        aarch64|arm64) echo "arm64-linux" ;;
        *) echo "x64-linux" ;;
      esac
      ;;
    Darwin)
      case "$(uname -m)" in
        arm64) echo "arm64-osx" ;;
        *) echo "x64-osx" ;;
      esac
      ;;
    *)
      echo "Unsupported OS: $(uname -s)" >&2
      exit 1
      ;;
  esac
}

TRIPLET="$(default_vcpkg_triplet)"
VCPKG_INSTALLED="$HERE/vcpkg_installed/$TRIPLET"

if ! opencv_ok "$CMAKE_PREFIX_PATH"; then
  CMAKE_PREFIX_PATH=""
fi
if [[ -z "$CMAKE_PREFIX_PATH" ]] && opencv_ok "$VCPKG_INSTALLED"; then
  CMAKE_PREFIX_PATH="$VCPKG_INSTALLED"
fi

if [[ -z "$LIBOBS_INCLUDE_DIR" ]]; then
  echo "Set --libobs-include or LIBOBS_INCLUDE_DIR (folder with obs.h)." >&2
  exit 1
fi
if [[ ! -d "$LIBOBS_INCLUDE_DIR" ]]; then
  echo "LIBOBS include dir not found: $LIBOBS_INCLUDE_DIR" >&2
  exit 1
fi
if [[ -z "$LIBOBS_LIB" ]]; then
  echo "Set --libobs-lib or LIBOBS_LIB (full path to libobs.so or libobs.dylib)." >&2
  exit 1
fi
if [[ ! -f "$LIBOBS_LIB" ]]; then
  echo "libobs library not found: $LIBOBS_LIB" >&2
  exit 1
fi

TOOLCHAIN=""
if [[ "$USE_VCPKG" -eq 1 ]]; then
  if [[ -z "$VCPKG_ROOT" ]]; then
    if [[ -x "$HERE/tools/vcpkg/vcpkg" ]]; then
      VCPKG_ROOT="$HERE/tools/vcpkg"
    fi
  fi
  if [[ -z "$VCPKG_ROOT" ]]; then
    VCPKG_ROOT="$HERE/tools/vcpkg"
    if [[ ! -x "$VCPKG_ROOT/vcpkg" ]]; then
      if ! command -v git >/dev/null 2>&1; then
        echo "Git required to clone vcpkg. Install git or set VCPKG_ROOT." >&2
        exit 1
      fi
      mkdir -p "$(dirname "$VCPKG_ROOT")"
      echo "Cloning vcpkg into $VCPKG_ROOT (one-time)..."
      git clone --depth 1 https://github.com/microsoft/vcpkg.git "$VCPKG_ROOT"
      echo "Bootstrapping vcpkg..."
      (cd "$VCPKG_ROOT" && ./bootstrap-vcpkg.sh -disableMetrics)
    fi
  fi
  TOOLCHAIN="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
  if [[ ! -f "$TOOLCHAIN" ]]; then
    echo "vcpkg toolchain not found: $TOOLCHAIN" >&2
    exit 1
  fi
elif [[ -z "$CMAKE_PREFIX_PATH" ]] || ! opencv_ok "$CMAKE_PREFIX_PATH"; then
  echo "OpenCV not found. Set CMAKE_PREFIX_PATH / --opencv, or run with --use-vcpkg." >&2
  echo "Example: ./build.sh --use-vcpkg --libobs-include .../libobs --libobs-lib .../libobs.so" >&2
  exit 1
fi

if ! command -v cmake >/dev/null 2>&1; then
  echo "cmake not found on PATH." >&2
  exit 1
fi

CMAKE_ARGS=(
  -B build
  -DCMAKE_BUILD_TYPE=Release
  "-DLIBOBS_INCLUDE_DIR=$LIBOBS_INCLUDE_DIR"
  "-DLIBOBS_LIB=$LIBOBS_LIB"
)

if [[ -n "$TOOLCHAIN" ]]; then
  echo "Using vcpkg toolchain: $TOOLCHAIN (triplet: $TRIPLET, OpenCV from vcpkg.json)"
  CMAKE_ARGS+=("-DCMAKE_TOOLCHAIN_FILE=$TOOLCHAIN" "-DVCPKG_TARGET_TRIPLET=$TRIPLET")
elif opencv_ok "$CMAKE_PREFIX_PATH"; then
  echo "Using OpenCV from: $CMAKE_PREFIX_PATH"
  CMAKE_ARGS+=("-DCMAKE_PREFIX_PATH=$CMAKE_PREFIX_PATH")
else
  echo "OpenCV prefix resolution failed." >&2
  exit 1
fi

echo "Configuring..."
cmake "${CMAKE_ARGS[@]}"

echo "Building Release..."
cmake --build build --parallel

echo ""
case "$(uname -s)" in
  Darwin) echo "Done. Typical output: $HERE/build/obs_license_plate_blur.dylib" ;;
  *)      echo "Done. Typical output: $HERE/build/obs_license_plate_blur.so" ;;
esac
