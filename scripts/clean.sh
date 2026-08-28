#!/usr/bin/env bash
# Removes the out-of-source build directory.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"

if [ -d "$BUILD_DIR" ]; then
    rm -rf "$BUILD_DIR"
    echo "Removed $BUILD_DIR"
else
    echo "Nothing to clean."
fi
