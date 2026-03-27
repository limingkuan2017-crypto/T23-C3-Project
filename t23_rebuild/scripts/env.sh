#!/usr/bin/env bash
set -e

# -----------------------------------------------------------------------------
# env.sh
#
# Role:
#   Export the most important path variables used by the T23 rebuild project.
#
# Why this file matters:
#   The original bring-up work happened in /home/kuan with local symlinks such
#   as t23_rebuild/sdk and t23_rebuild/vendor_ref. For a portable repository we
#   instead resolve paths relative to the repository root:
#     <repo>/t23_rebuild
#     <repo>/third_party/ingenic_t23_sdk
#     <repo>/third_party/vendor_reference
#
# Usage:
#   source ./scripts/env.sh
# -----------------------------------------------------------------------------

export T23_REBUILD_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
export T23_REPO_ROOT=$(cd "$T23_REBUILD_ROOT/.." && pwd)
export T23_SDK_ROOT=${T23_SDK_ROOT:-"$T23_REPO_ROOT/third_party/ingenic_t23_sdk"}
export T23_VENDOR_REF=${T23_VENDOR_REF:-"$T23_REPO_ROOT/third_party/vendor_reference"}
export T23_CAMERA_APP="$T23_REBUILD_ROOT/app/camera"
export T23_OUTPUT_ROOT="$T23_REBUILD_ROOT/output"

echo "T23_REPO_ROOT=$T23_REPO_ROOT"
echo "T23_REBUILD_ROOT=$T23_REBUILD_ROOT"
echo "T23_SDK_ROOT=$T23_SDK_ROOT"
echo "T23_VENDOR_REF=$T23_VENDOR_REF"
