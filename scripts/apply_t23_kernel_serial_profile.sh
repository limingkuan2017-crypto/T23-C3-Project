#!/usr/bin/env bash
set -euo pipefail

# -----------------------------------------------------------------------------
# apply_t23_kernel_serial_profile.sh
#
# Role:
#   Apply a small board-profile config fragment to the Ingenic SDK kernel
#   .config, so we can switch between different serial topologies without
#   manually editing the SDK every time.
#
# Why this exists:
#   The SDK kernel .config lives outside our first-party code, but different
#   hardware revisions need different UART enable/pinmux choices.
#   Keeping tiny profile files in the repository is safer than relying on
#   memory or repeatedly editing the huge .config file by hand.
#
# Usage:
#   ./scripts/apply_t23_kernel_serial_profile.sh t23_new_hw
#   ./scripts/apply_t23_kernel_serial_profile.sh t23_vendor_hw
#
# What it edits:
#   <repo>/third_party/ingenic_t23_sdk/opensource/kernel/.config
# -----------------------------------------------------------------------------

REPO_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
PROFILE_NAME=${1:-}

if [ -z "$PROFILE_NAME" ]; then
    echo "usage: $0 <profile-name>"
    echo
    echo "available profiles:"
    echo "  t23_new_hw"
    echo "  t23_vendor_hw"
    exit 1
fi

PROFILE_PATH="$REPO_ROOT/configs/${PROFILE_NAME}_kernel_serial.conf"
KERNEL_CONFIG="$REPO_ROOT/third_party/ingenic_t23_sdk/opensource/kernel/.config"

if [ ! -f "$PROFILE_PATH" ]; then
    echo "profile not found: $PROFILE_PATH"
    exit 1
fi

if [ ! -f "$KERNEL_CONFIG" ]; then
    echo "kernel config not found: $KERNEL_CONFIG"
    exit 1
fi

get_profile_value() {
    local key="$1"
    local line
    line=$(grep -E "^${key}=" "$PROFILE_PATH" | tail -n 1 || true)
    if [ -z "$line" ]; then
        echo "missing key in profile: $key" >&2
        exit 1
    fi
    printf '%s' "${line#*=}" | tr -d '\r'
}

# shellcheck disable=SC1090
source "$PROFILE_PATH"

set_bool() {
    local key="$1"
    local value="$2"

    sed -i "/^${key}=y$/d;/^${key}=n$/d;/^# ${key} is not set$/d" "$KERNEL_CONFIG"

    if [ "$value" = "y" ]; then
        echo "${key}=y" >> "$KERNEL_CONFIG"
    else
        echo "# ${key} is not set" >> "$KERNEL_CONFIG"
    fi
}

echo "Applying T23 kernel serial profile:"
echo "  name : ${PROFILE_NAME}"
echo "  desc : ${PROFILE_DESC}"
echo "  file : ${PROFILE_PATH}"
echo "  target config : ${KERNEL_CONFIG}"

set_bool CONFIG_SERIAL_T23_UART0 "$(get_profile_value CONFIG_SERIAL_T23_UART0)"
set_bool CONFIG_SERIAL_T23_UART0_DMA "$(get_profile_value CONFIG_SERIAL_T23_UART0_DMA)"
set_bool CONFIG_UART0_PB "$(get_profile_value CONFIG_UART0_PB)"
set_bool CONFIG_UART0_PB_FC "$(get_profile_value CONFIG_UART0_PB_FC)"
set_bool CONFIG_SERIAL_T23_UART1 "$(get_profile_value CONFIG_SERIAL_T23_UART1)"
set_bool CONFIG_SERIAL_T23_UART1_DMA "$(get_profile_value CONFIG_SERIAL_T23_UART1_DMA)"
set_bool CONFIG_UART1_PA "$(get_profile_value CONFIG_UART1_PA)"
set_bool CONFIG_UART1_PB "$(get_profile_value CONFIG_UART1_PB)"

echo
echo "Result snapshot:"
grep -n -A12 -B4 'CONFIG_SERIAL_T23_UART0\|CONFIG_SERIAL_T23_UART1' "$KERNEL_CONFIG"
