#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

run_timeout_config() {
    env "$@" VGPU3D_PRINT_TIMEOUTS=1 "$REPO_ROOT/.ci/device-smoke/test-gpu-3d.sh"
}

get_value() {
    local key="$1"
    awk -F= -v key="$key" '$1 == key { print $2 }'
}

output="$(run_timeout_config VGPU3D_GLXGEARS_RUNS=10 VGPU3D_XORG_RESTARTS=2)"
cmd_timeout="$(get_value VGPU3D_CMD_TIMEOUT <<<"$output")"
repeat_timeout="$(get_value VGPU3D_REPEAT_TIMEOUT <<<"$output")"

if [[ "$cmd_timeout" != 180 ]]; then
    printf 'expected default VGPU3D_CMD_TIMEOUT=180, got %s\n' "$cmd_timeout"
    exit 1
fi
if (( repeat_timeout <= cmd_timeout )); then
    printf 'expected repeat timeout (%s) to exceed command timeout (%s)\n' \
        "$repeat_timeout" "$cmd_timeout"
    exit 1
fi

output="$(run_timeout_config VGPU3D_CMD_TIMEOUT=900)"
repeat_timeout="$(get_value VGPU3D_REPEAT_TIMEOUT <<<"$output")"
if [[ "$repeat_timeout" != 900 ]]; then
    printf 'expected high VGPU3D_CMD_TIMEOUT to raise repeat timeout to 900, got %s\n' \
        "$repeat_timeout"
    exit 1
fi

output="$(run_timeout_config VGPU3D_REPEAT_TIMEOUT=77)"
repeat_timeout="$(get_value VGPU3D_REPEAT_TIMEOUT <<<"$output")"
if [[ "$repeat_timeout" != 77 ]]; then
    printf 'expected explicit VGPU3D_REPEAT_TIMEOUT=77, got %s\n' "$repeat_timeout"
    exit 1
fi

repeat_expect_block="$(
    awk '
        /set timeout \$env\(VGPU3D_REPEAT_TIMEOUT\)/ { in_block = 1 }
        in_block { print }
        /set timeout \$env\(VGPU3D_CMD_TIMEOUT\)/ && in_block { exit }
    ' "$REPO_ROOT/.ci/device-smoke/test-gpu-3d.sh"
)"
if ! grep -q 'timeout { exit 10 }' <<<"$repeat_expect_block"; then
    printf '%s\n' 'expected repeat sentinel timeout to exit 10'
    exit 1
fi
if ! grep -q 'eof { exit 10 }' <<<"$repeat_expect_block"; then
    printf '%s\n' 'expected repeat sentinel EOF to exit 10'
    exit 1
fi

if ! grep -q 'xorg_glx_ready' "$REPO_ROOT/.ci/device-smoke/test-gpu-3d.sh"; then
    printf '%s\n' 'expected vgpu 3D smoke to wait for Xorg GLX readiness'
    exit 1
fi
