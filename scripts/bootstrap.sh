#!/usr/bin/env bash
set -euo pipefail

# -----------------------------------------------------------------------------
# bootstrap.sh
#
# Role:
#   Repository-level helper for first-time setup checks and common build steps.
#
# Why this script exists:
#   After moving the T23, C3 and shared code into one monorepo, the next pain
#   point is "what do I run on a fresh machine?" This script gives one stable
#   entry point so a new developer does not need to memorize several separate
#   commands.
#
# Supported actions:
#   --check        only verify repository layout and external dependencies
#   --build-t23    build T23 camera and SPI diagnostic binaries
#   --package-t23  generate the flashable T23 image package
#   --build-c3     build the ESP32-C3 firmware
#   --all          run check + T23 build + T23 package + C3 build
#
# Default behavior:
#   If no option is given, the script runs --check.
# -----------------------------------------------------------------------------

REPO_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
CHECK_ONLY=0
BUILD_T23=0
PACKAGE_T23=0
BUILD_C3=0

usage() {
    cat <<EOF
usage: $0 [--check] [--build-t23] [--package-t23] [--build-c3] [--all]

examples:
  $0 --check
  $0 --build-t23
  $0 --package-t23
  $0 --build-c3
  $0 --all
EOF
}

run_check() {
    echo
    echo "[1/4] checking workspace"
    "$REPO_ROOT/scripts/check_workspace.sh"
}

run_build_t23() {
    echo
    echo "[2/4] building T23 diagnostics"
    (cd "$REPO_ROOT/t23_rebuild" && ./scripts/build_camera.sh)
    (cd "$REPO_ROOT/t23_rebuild/app/spi_diag" && make clean all)
}

run_package_t23() {
    echo
    echo "[3/4] packaging T23 flash image"
    (cd "$REPO_ROOT/t23_rebuild" && ./scripts/package_flash_image.sh)
}

run_build_c3() {
    echo
    echo "[4/4] building ESP32-C3 firmware"
    (cd "$REPO_ROOT/c3_rebuild" && ./scripts/idf_build.sh)
}

if [ $# -eq 0 ]; then
    CHECK_ONLY=1
fi

while [ $# -gt 0 ]; do
    case "$1" in
        --check)
            CHECK_ONLY=1
            ;;
        --build-t23)
            BUILD_T23=1
            ;;
        --package-t23)
            PACKAGE_T23=1
            ;;
        --build-c3)
            BUILD_C3=1
            ;;
        --all)
            CHECK_ONLY=1
            BUILD_T23=1
            PACKAGE_T23=1
            BUILD_C3=1
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "error: unknown option: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
    shift
done

if [ "$CHECK_ONLY" -eq 1 ]; then
    run_check
fi

if [ "$BUILD_T23" -eq 1 ]; then
    run_build_t23
fi

if [ "$PACKAGE_T23" -eq 1 ]; then
    run_package_t23
fi

if [ "$BUILD_C3" -eq 1 ]; then
    run_build_c3
fi

echo
echo "bootstrap completed"
