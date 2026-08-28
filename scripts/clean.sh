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

# imgui.ini and pipeline_cache.bin write next to wherever the exe was run
# from, not necessarily build/<preset>/bin - sweep stray copies at repo
# root, the common case when the exe is run with the repo root as cwd.
for name in imgui.ini pipeline_cache.bin; do
    if [ -f "$ROOT_DIR/$name" ]; then
        rm -f "$ROOT_DIR/$name"
        echo "Removed $ROOT_DIR/$name"
    fi
done
find "$ROOT_DIR" -maxdepth 1 -name "*.ilk" -type f -print -delete
if [ -d "$ROOT_DIR/pdb" ]; then
    rm -rf "$ROOT_DIR/pdb"
    echo "Removed $ROOT_DIR/pdb"
fi
