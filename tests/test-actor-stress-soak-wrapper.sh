#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

WRAPPER="${REPO_ROOT}/.ci/device-smoke/test-actor-stress-all-soak.sh"
ACTOR_STRESS="${REPO_ROOT}/.ci/device-smoke/test-actor-stress.sh"

get_value() {
    local key="$1"
    awk -F= -v key="$key" '$1 == key { print $2 }'
}

run_wrapper_config() {
    env "$@" SEMU_ACTOR_STRESS_SOAK_PRINT_CONFIG=1 "${WRAPPER}"
}

run_actor_config() {
    env "$@" SEMU_ACTOR_STRESS_SOAK_PRINT_CONFIG=1 "${ACTOR_STRESS}"
}

output="$(run_wrapper_config)"
runs="$(get_value SEMU_ACTOR_STRESS_RUNS <<<"${output}")"
tests="$(get_value SEMU_ACTOR_STRESS_TESTS <<<"${output}")"
actor_stress_script="$(get_value SEMU_ACTOR_STRESS_SCRIPT <<<"${output}")"

if [[ "${runs}" != 5 ]]; then
    printf 'expected all-device actor soak to default SEMU_ACTOR_STRESS_RUNS=5, got %s\n' \
        "${runs}"
    exit 1
fi
if [[ "${tests}" != "gpu vinput netdev sound" ]]; then
    printf 'expected all-device actor soak default tests, got %s\n' "${tests}"
    exit 1
fi
if [[ "${actor_stress_script}" != "${ACTOR_STRESS}" ]]; then
    printf 'expected actor stress script %s, got %s\n' \
        "${ACTOR_STRESS}" "${actor_stress_script}"
    exit 1
fi

output="$(
    run_wrapper_config \
        SEMU_ACTOR_STRESS_RUNS=7 \
        SEMU_ACTOR_STRESS_TESTS="gpu sound"
)"
runs="$(get_value SEMU_ACTOR_STRESS_RUNS <<<"${output}")"
tests="$(get_value SEMU_ACTOR_STRESS_TESTS <<<"${output}")"
if [[ "${runs}" != 7 ]]; then
    printf 'expected all-device actor soak to preserve SEMU_ACTOR_STRESS_RUNS=7, got %s\n' \
        "${runs}"
    exit 1
fi
if [[ "${tests}" != "gpu sound" ]]; then
    printf 'expected all-device actor soak to preserve custom tests, got %s\n' "${tests}"
    exit 1
fi

output="$(run_actor_config SEMU_ACTOR_STRESS_RUNS=5 SEMU_ACTOR_STRESS_TESTS=all-soak)"
config_count="$(grep -c '^SEMU_ACTOR_STRESS_RUNS=' <<<"${output}")"
if [[ "${config_count}" != 1 ]]; then
    printf 'expected top-level all-soak handoff to print one config, got %s\n' \
        "${config_count}"
    exit 1
fi
if grep -q '^=== actor stress run ' <<<"${output}"; then
    printf '%s\n' 'expected top-level all-soak handoff to bypass outer actor loop'
    exit 1
fi
runs="$(get_value SEMU_ACTOR_STRESS_RUNS <<<"${output}")"
tests="$(get_value SEMU_ACTOR_STRESS_TESTS <<<"${output}")"
if [[ "${runs}" != 5 ]]; then
    printf 'expected top-level all-soak handoff to preserve SEMU_ACTOR_STRESS_RUNS=5, got %s\n' \
        "${runs}"
    exit 1
fi
if [[ "${tests}" != "gpu vinput netdev sound" ]]; then
    printf 'expected top-level all-soak handoff to normalize to all-device tests, got %s\n' \
        "${tests}"
    exit 1
fi

set +e
mixed_output="$(run_wrapper_config SEMU_ACTOR_STRESS_TESTS="gpu all-soak" 2>&1)"
mixed_status=$?
set -e
if (( mixed_status == 0 )); then
    printf '%s\n' 'expected mixed all-soak wrapper input to fail'
    exit 1
fi
if ! grep -q 'all-soak must be the only actor stress test' <<<"${mixed_output}"; then
    printf '%s\n' 'expected mixed all-soak failure message'
    exit 1
fi

set +e
mixed_actor_output="$(run_actor_config SEMU_ACTOR_STRESS_TESTS="gpu all-soak" 2>&1)"
mixed_actor_status=$?
set -e
if (( mixed_actor_status == 0 )); then
    printf '%s\n' 'expected mixed all-soak actor dispatcher input to fail'
    exit 1
fi
if ! grep -q 'all-soak must be the only actor stress test' <<<"${mixed_actor_output}"; then
    printf '%s\n' 'expected mixed all-soak dispatcher failure message'
    exit 1
fi

set +e
invalid_output="$(run_wrapper_config SEMU_ACTOR_STRESS_RUNS=0 2>&1)"
invalid_status=$?
set -e
if (( invalid_status == 0 )); then
    printf '%s\n' 'expected all-device actor soak to reject SEMU_ACTOR_STRESS_RUNS=0'
    exit 1
fi
if ! grep -q 'SEMU_ACTOR_STRESS_RUNS must be a positive integer' <<<"${invalid_output}"; then
    printf '%s\n' 'expected positive integer validation message for soak runs'
    exit 1
fi

if ! grep -q 'all-soak)' "${ACTOR_STRESS}"; then
    printf '%s\n' 'expected actor stress dispatcher to support all-soak'
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
if ! grep -q 'all-soak' <<<"${actor_output}"; then
    printf '%s\n' 'expected actor stress known-tests help to include all-soak'
    exit 1
fi
