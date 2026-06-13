#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

export HEADLESS=0
: "${VGPU3D_GLXGEARS_RUNS:=12}"
: "${VGPU3D_XORG_RESTARTS:=3}"
export VGPU3D_GLXGEARS_RUNS
export VGPU3D_XORG_RESTARTS

STRESS_SCRIPT="${SCRIPT_DIR}/test-gpu-3d-stress.sh"

if [[ "${VGPU3D_SOAK_PRINT_CONFIG:-0}" == 1 ]]; then
    timeout_output="$(VGPU3D_PRINT_TIMEOUTS=1 "${STRESS_SCRIPT}")"
    printf 'HEADLESS=%s\n' "${HEADLESS}"
    printf 'VGPU3D_GLXGEARS_RUNS=%s\n' "${VGPU3D_GLXGEARS_RUNS}"
    printf 'VGPU3D_XORG_RESTARTS=%s\n' "${VGPU3D_XORG_RESTARTS}"
    printf '%s\n' "${timeout_output}"
    printf 'VGPU3D_STRESS_SCRIPT=%s\n' "${STRESS_SCRIPT}"
    exit 0
fi

exec "${STRESS_SCRIPT}" "$@"
