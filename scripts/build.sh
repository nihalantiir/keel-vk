#!/usr/bin/env bash
# Configures and builds keel-vk out-of-source with CMake + Ninja.
# Usage: ./scripts/build.sh [Debug|Release|RelWithDebInfo]
set -euo pipefail

CONFIG="${1:-Debug}"
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"

mkdir -p "$BUILD_DIR"

cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE="$CONFIG"
cmake --build "$BUILD_DIR" --config "$CONFIG"
