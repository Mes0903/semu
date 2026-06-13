#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

WRAPPER="${REPO_ROOT}/.ci/device-smoke/test-gpu-3d-reset-stress-long.sh"
ACTOR_STRESS="${REPO_ROOT}/.ci/device-smoke/test-actor-stress.sh"
REBOOT_STRESS="${REPO_ROOT}/.ci/device-smoke/test-gpu-3d-reboot-stress.sh"
WINDOW_CLOSE_STRESS="${REPO_ROOT}/.ci/device-smoke/test-gpu-3d-window-close-stress.sh"

get_value() {
    local key="$1"
    awk -F= -v key="$key" '$1 == key { print $2 }'
}

run_wrapper_config() {
    env "$@" VGPU3D_RESET_STRESS_PRINT_CONFIG=1 "${WRAPPER}"
}

output="$(run_wrapper_config)"
headless="$(get_value HEADLESS <<<"${output}")"
reboot_runs="$(get_value VGPU3D_REBOOT_RUNS <<<"${output}")"
window_close_runs="$(get_value VGPU3D_WINDOW_CLOSE_RUNS <<<"${output}")"
reboot_script="$(get_value VGPU3D_REBOOT_SCRIPT <<<"${output}")"
window_close_script="$(get_value VGPU3D_WINDOW_CLOSE_SCRIPT <<<"${output}")"

if [[ "${headless}" != 0 ]]; then
    printf 'expected reset stress wrapper to default HEADLESS=0, got %s\n' "${headless}"
    exit 1
fi
if [[ "${reboot_runs}" != 2 ]]; then
    printf 'expected reset stress wrapper to default VGPU3D_REBOOT_RUNS=2, got %s\n' "${reboot_runs}"
    exit 1
fi
if [[ "${window_close_runs}" != 2 ]]; then
    printf 'expected reset stress wrapper to default VGPU3D_WINDOW_CLOSE_RUNS=2, got %s\n' "${window_close_runs}"
    exit 1
fi
if [[ "${reboot_script}" != "${REBOOT_STRESS}" ]]; then
    printf 'expected reboot script %s, got %s\n' "${REBOOT_STRESS}" "${reboot_script}"
    exit 1
fi
if [[ "${window_close_script}" != "${WINDOW_CLOSE_STRESS}" ]]; then
    printf 'expected window-close script %s, got %s\n' \
        "${WINDOW_CLOSE_STRESS}" "${window_close_script}"
    exit 1
fi

output="$(
    run_wrapper_config \
        HEADLESS=1 \
        VGPU3D_REBOOT_RUNS=5 \
        VGPU3D_WINDOW_CLOSE_RUNS=4
)"
headless="$(get_value HEADLESS <<<"${output}")"
reboot_runs="$(get_value VGPU3D_REBOOT_RUNS <<<"${output}")"
window_close_runs="$(get_value VGPU3D_WINDOW_CLOSE_RUNS <<<"${output}")"

if [[ "${headless}" != 0 ]]; then
    printf 'expected reset stress wrapper to force visible HEADLESS=0, got %s\n' "${headless}"
    exit 1
fi
if [[ "${reboot_runs}" != 5 ]]; then
    printf 'expected reset stress wrapper to preserve VGPU3D_REBOOT_RUNS=5, got %s\n' \
        "${reboot_runs}"
    exit 1
fi
if [[ "${window_close_runs}" != 4 ]]; then
    printf 'expected reset stress wrapper to preserve VGPU3D_WINDOW_CLOSE_RUNS=4, got %s\n' \
        "${window_close_runs}"
    exit 1
fi

if ! grep -q 'gpu3d-reset-long)' "${ACTOR_STRESS}"; then
    printf '%s\n' 'expected actor stress dispatcher to support gpu3d-reset-long'
    exit 1
fi

set +e
actor_output="$(
    SEMU_ACTOR_STRESS_TESTS=__definitely_unknown__ \
        "${ACTOR_STRESS}" 2>&1
)"
actor_status=$?
set -e
if (( actor_status == 0 )); then
    printf '%s\n' 'expected actor stress unknown selector validation to fail'
    exit 1
fi
if ! grep -q 'gpu3d-reset-long' <<<"${actor_output}"; then
    printf '%s\n' 'expected actor stress known-tests help to include gpu3d-reset-long'
    exit 1
fi
