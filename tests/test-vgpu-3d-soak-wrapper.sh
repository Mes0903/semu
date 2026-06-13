#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

WRAPPER="${REPO_ROOT}/.ci/device-smoke/test-gpu-3d-soak.sh"
ACTOR_STRESS="${REPO_ROOT}/.ci/device-smoke/test-actor-stress.sh"
STRESS_SCRIPT="${REPO_ROOT}/.ci/device-smoke/test-gpu-3d-stress.sh"

get_value() {
    local key="$1"
    awk -F= -v key="$key" '$1 == key { print $2 }'
}

run_wrapper_config() {
    env "$@" VGPU3D_SOAK_PRINT_CONFIG=1 "${WRAPPER}"
}

output="$(run_wrapper_config)"
headless="$(get_value HEADLESS <<<"${output}")"
glxgears_runs="$(get_value VGPU3D_GLXGEARS_RUNS <<<"${output}")"
xorg_restarts="$(get_value VGPU3D_XORG_RESTARTS <<<"${output}")"
stress_seconds="$(get_value VGPU3D_STRESS_GLXGEARS_SECONDS <<<"${output}")"
repeat_timeout="$(get_value VGPU3D_REPEAT_TIMEOUT <<<"${output}")"
stress_script="$(get_value VGPU3D_STRESS_SCRIPT <<<"${output}")"

if [[ "${headless}" != 0 ]]; then
    printf 'expected vgpu 3D soak wrapper to default HEADLESS=0, got %s\n' "${headless}"
    exit 1
fi
if [[ "${glxgears_runs}" != 12 ]]; then
    printf 'expected vgpu 3D soak wrapper to default VGPU3D_GLXGEARS_RUNS=12, got %s\n' \
        "${glxgears_runs}"
    exit 1
fi
if [[ "${xorg_restarts}" != 3 ]]; then
    printf 'expected vgpu 3D soak wrapper to default VGPU3D_XORG_RESTARTS=3, got %s\n' \
        "${xorg_restarts}"
    exit 1
fi
if [[ -z "${stress_seconds}" ]]; then
    printf '%s\n' 'expected vgpu 3D soak wrapper to print VGPU3D_STRESS_GLXGEARS_SECONDS'
    exit 1
fi
if (( repeat_timeout <= 690 )); then
    printf 'expected vgpu 3D soak repeat timeout (%s) to exceed previous 10/2 timeout 690\n' \
        "${repeat_timeout}"
    exit 1
fi
if [[ "${stress_script}" != "${STRESS_SCRIPT}" ]]; then
    printf 'expected stress script %s, got %s\n' "${STRESS_SCRIPT}" "${stress_script}"
    exit 1
fi

output="$(
    run_wrapper_config \
        HEADLESS=1 \
        VGPU3D_GLXGEARS_RUNS=13 \
        VGPU3D_XORG_RESTARTS=4 \
        VGPU3D_STRESS_GLXGEARS_SECONDS=9 \
        VGPU3D_REPEAT_TIMEOUT=777
)"
headless="$(get_value HEADLESS <<<"${output}")"
glxgears_runs="$(get_value VGPU3D_GLXGEARS_RUNS <<<"${output}")"
xorg_restarts="$(get_value VGPU3D_XORG_RESTARTS <<<"${output}")"
stress_seconds="$(get_value VGPU3D_STRESS_GLXGEARS_SECONDS <<<"${output}")"
repeat_timeout="$(get_value VGPU3D_REPEAT_TIMEOUT <<<"${output}")"

if [[ "${headless}" != 0 ]]; then
    printf 'expected vgpu 3D soak wrapper to force visible HEADLESS=0, got %s\n' "${headless}"
    exit 1
fi
if [[ "${glxgears_runs}" != 13 ]]; then
    printf 'expected vgpu 3D soak wrapper to preserve VGPU3D_GLXGEARS_RUNS=13, got %s\n' \
        "${glxgears_runs}"
    exit 1
fi
if [[ "${xorg_restarts}" != 4 ]]; then
    printf 'expected vgpu 3D soak wrapper to preserve VGPU3D_XORG_RESTARTS=4, got %s\n' \
        "${xorg_restarts}"
    exit 1
fi
if [[ "${stress_seconds}" != 9 ]]; then
    printf 'expected vgpu 3D soak wrapper to preserve VGPU3D_STRESS_GLXGEARS_SECONDS=9, got %s\n' \
        "${stress_seconds}"
    exit 1
fi
if [[ "${repeat_timeout}" != 777 ]]; then
    printf 'expected vgpu 3D soak wrapper to preserve VGPU3D_REPEAT_TIMEOUT=777, got %s\n' \
        "${repeat_timeout}"
    exit 1
fi

if ! grep -q 'gpu3d-soak)' "${ACTOR_STRESS}"; then
    printf '%s\n' 'expected actor stress dispatcher to support gpu3d-soak'
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
if ! grep -q 'gpu3d-soak' <<<"${actor_output}"; then
    printf '%s\n' 'expected actor stress known-tests help to include gpu3d-soak'
    exit 1
fi
