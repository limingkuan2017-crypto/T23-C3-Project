#!/usr/bin/env bash
set -euo pipefail

if [ $# -lt 1 ]; then
    echo "usage: $0 <port>"
    exit 2
fi

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
PORT=$1
BAUD=${ESPTOOL_BAUD:-115200}
BEFORE=${ESPTOOL_BEFORE:-default_reset}
AFTER=${ESPTOOL_AFTER:-hard_reset}

. "$ROOT/scripts/idf_env.sh"

cd "$ROOT"
# This path assumes the serial adapter can control reset/boot mode automatically.
idf.py -p "$PORT" -b "$BAUD" \
    -D ESPTOOLPY_BEFORE="$BEFORE" \
    -D ESPTOOLPY_AFTER="$AFTER" \
    flash monitor
