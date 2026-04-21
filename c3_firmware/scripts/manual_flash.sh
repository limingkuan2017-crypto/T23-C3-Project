#!/usr/bin/env bash
set -euo pipefail

if [ $# -lt 1 ]; then
    echo "usage: $0 <port>"
    exit 2
fi

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
PORT=$1
BAUD=${ESPTOOL_BAUD:-115200}

. "$ROOT/scripts/idf_env.sh"

cd "$ROOT"

echo "Manual download mode flash"
echo "1. Hold BOOT."
echo "2. Press and release EN/RESET."
echo "3. Keep BOOT held for about 1 second, then release."
echo "4. Press Enter here when the chip is in download mode."
read -r _

# Use no_reset/no_reset because the connected adapter may not have working
# RTS/DTR lines for automatic ESP32-C3 download mode control.
idf.py -p "$PORT" -b "$BAUD" \
    -D ESPTOOLPY_BEFORE=no_reset \
    -D ESPTOOLPY_AFTER=no_reset \
    flash
