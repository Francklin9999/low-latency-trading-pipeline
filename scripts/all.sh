#!/usr/bin/env bash
# Build everything, then launch the full tuned stack. Usage: sudo ./scripts/all.sh
# Override any HFT_* env var (see scripts/run/all.sh CONFIG block).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"

if [[ ! -f third_party/orderbook/CMakeLists.txt ]]; then
    echo "[all] initializing third_party submodules"
    git submodule update --init --recursive
fi

exec "$SCRIPT_DIR/run/all.sh" "$@"
