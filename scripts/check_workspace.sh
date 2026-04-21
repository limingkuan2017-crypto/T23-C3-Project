#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

echo "repo root        : $REPO_ROOT"
echo "t23 project      : $REPO_ROOT/t23_firmware"
echo "c3 project       : $REPO_ROOT/c3_firmware"
echo "shared project   : $REPO_ROOT/shared"
echo "sdk dependency   : $REPO_ROOT/third_party/ingenic_t23_sdk"
echo "vendor reference : $REPO_ROOT/third_party/vendor_reference"
echo

for p in \
    "$REPO_ROOT/t23_firmware" \
    "$REPO_ROOT/c3_firmware" \
    "$REPO_ROOT/shared"; do
    if [ -d "$p" ]; then
        echo "[ok] found $p"
    else
        echo "[missing] $p"
    fi
done

for p in \
    "$REPO_ROOT/third_party/ingenic_t23_sdk" \
    "$REPO_ROOT/third_party/vendor_reference"; do
    if [ -e "$p" ]; then
        echo "[ok] found $p"
    else
        echo "[missing] $p"
    fi
done
