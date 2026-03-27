#!/usr/bin/env bash
set -euo pipefail

# Resolve ESP-IDF in a portable way.
#
# Preferred order:
# 1. Respect an existing IDF_PATH from the caller's environment.
# 2. Fall back to the version already used during project bring-up.
# 3. Exit with a clear error if ESP-IDF cannot be found.
if [ -n "${IDF_PATH:-}" ] && [ -f "${IDF_PATH}/export.sh" ]; then
    :
elif [ -f "$HOME/.espressif/v5.4.3/esp-idf/export.sh" ]; then
    export IDF_PATH="$HOME/.espressif/v5.4.3/esp-idf"
else
    echo "error: ESP-IDF not found. Set IDF_PATH before building." >&2
    exit 2
fi

. "$IDF_PATH/export.sh" >/dev/null
