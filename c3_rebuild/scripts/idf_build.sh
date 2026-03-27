#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
. "$ROOT/scripts/idf_env.sh"

cd "$ROOT"
# Re-run set-target so a clean checkout can build without extra manual setup.
idf.py set-target esp32c3 >/dev/null
idf.py build
