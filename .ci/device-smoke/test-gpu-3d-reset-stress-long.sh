#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

export HEADLESS=0
: "${VGPU3D_REBOOT_RUNS:=2}"
: "${VGPU3D_WINDOW_CLOSE_RUNS:=2}"

case "${VGPU3D_REBOOT_RUNS}" in
    ''|*[!0-9]*)
        echo "FAIL: VGPU3D_REBOOT_RUNS must be a positive integer" >&2
        exit 1
        ;;
esac
if (( 10#${VGPU3D_REBOOT_RUNS} < 1 )); then
    echo "FAIL: VGPU3D_REBOOT_RUNS must be a positive integer" >&2
    exit 1
fi

case "${VGPU3D_WINDOW_CLOSE_RUNS}" in
    ''|*[!0-9]*)
        echo "FAIL: VGPU3D_WINDOW_CLOSE_RUNS must be a positive integer" >&2
        exit 1
        ;;
esac
if (( 10#${VGPU3D_WINDOW_CLOSE_RUNS} < 1 )); then
    echo "FAIL: VGPU3D_WINDOW_CLOSE_RUNS must be a positive integer" >&2
    exit 1
fi

export VGPU3D_REBOOT_RUNS VGPU3D_WINDOW_CLOSE_RUNS

REBOOT_SCRIPT="${SCRIPT_DIR}/test-gpu-3d-reboot-stress.sh"
WINDOW_CLOSE_SCRIPT="${SCRIPT_DIR}/test-gpu-3d-window-close-stress.sh"

if [[ "${VGPU3D_RESET_STRESS_PRINT_CONFIG:-0}" == 1 ]]; then
    printf 'HEADLESS=%s\n' "${HEADLESS}"
    printf 'VGPU3D_REBOOT_RUNS=%s\n' "${VGPU3D_REBOOT_RUNS}"
    printf 'VGPU3D_WINDOW_CLOSE_RUNS=%s\n' "${VGPU3D_WINDOW_CLOSE_RUNS}"
    printf 'VGPU3D_REBOOT_SCRIPT=%s\n' "${REBOOT_SCRIPT}"
    printf 'VGPU3D_WINDOW_CLOSE_SCRIPT=%s\n' "${WINDOW_CLOSE_SCRIPT}"
    exit 0
fi

"${REBOOT_SCRIPT}"

for (( run = 1; run <= 10#${VGPU3D_WINDOW_CLOSE_RUNS}; run++ )); do
    printf '\n=== vgpu 3D window-close stress run %d/%d ===\n' \
        "${run}" "${VGPU3D_WINDOW_CLOSE_RUNS}"
    "${WINDOW_CLOSE_SCRIPT}"
done
