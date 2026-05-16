#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$REPO_ROOT"

build_type="${CMAKE_BUILD_TYPE:-Release}"

cmake -S . -B build -DCMAKE_BUILD_TYPE="$build_type"
cmake --build build -j

if command -v docker >/dev/null 2>&1; then
    docker compose build control
else
    echo "[build/all] docker not found; skipping control image build" >&2
fi
