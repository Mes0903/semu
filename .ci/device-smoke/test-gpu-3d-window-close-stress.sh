#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

: "${VGPU3D_GLXGEARS_RUNS:=1}"
: "${VGPU3D_XORG_RESTARTS:=0}"
: "${VGPU3D_WINDOW_CLOSE_WAIT_TIMEOUT:=30}"
: "${SEMU_TEST_WINDOW_CLOSE_AFTER_MS:=100}"

export HEADLESS=0
export VGPU3D_GLXGEARS_RUNS
export VGPU3D_XORG_RESTARTS
export VGPU3D_WINDOW_CLOSE_WAIT_TIMEOUT
export SEMU_TEST_WINDOW_CLOSE_AFTER_MS
export VGPU3D_EXPECT_WINDOW_CLOSE=1

arm_file="${TMPDIR:-/tmp}/semu-vgpu-window-close-$$"
rm -f "${arm_file}"
export SEMU_TEST_WINDOW_CLOSE_ARM_FILE="${arm_file}"

cleanup_window_close_arm_file() {
    rm -f "${arm_file}"
}
trap cleanup_window_close_arm_file EXIT

"${SCRIPT_DIR}/test-gpu-3d.sh"
