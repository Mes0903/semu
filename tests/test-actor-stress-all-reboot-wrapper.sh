#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

WRAPPER="${REPO_ROOT}/.ci/device-smoke/test-actor-stress-all-reboot.sh"
ACTOR_STRESS="${REPO_ROOT}/.ci/device-smoke/test-actor-stress.sh"

get_value() {
    local key="$1"
    awk -F= -v key="$key" '$1 == key { print $2 }'
}

run_wrapper_config() {
    env "$@" SEMU_ACTOR_CROSS_REBOOT_PRINT_CONFIG=1 "${WRAPPER}"
}

run_actor_config() {
    env "$@" SEMU_ACTOR_CROSS_REBOOT_PRINT_CONFIG=1 "${ACTOR_STRESS}"
}

expect_invalid_runs() {
    local value="$1"
    local output status

    set +e
    output="$(run_wrapper_config SEMU_ACTOR_CROSS_REBOOT_RUNS="${value}" 2>&1)"
    status=$?
    set -e

    if (( status == 0 )); then
        printf 'expected SEMU_ACTOR_CROSS_REBOOT_RUNS=%s to fail\n' "${value}"
        exit 1
    fi
    if ! grep -q 'SEMU_ACTOR_CROSS_REBOOT_RUNS must be a positive integer' <<<"${output}"; then
        printf 'expected positive integer validation for SEMU_ACTOR_CROSS_REBOOT_RUNS=%s\n' \
            "${value}"
        exit 1
    fi
}

output="$(run_wrapper_config)"
runs="$(get_value SEMU_ACTOR_CROSS_REBOOT_RUNS <<<"${output}")"
executor="$(get_value EXECUTOR <<<"${output}")"
smp="$(get_value SMP <<<"${output}")"
netdev="$(get_value NETDEV <<<"${output}")"
timeout="$(get_value TIMEOUT <<<"${output}")"
cmd_timeout="$(get_value SEMU_ACTOR_CROSS_REBOOT_CMD_TIMEOUT <<<"${output}")"
sound_sample="$(get_value SEMU_ACTOR_CROSS_SOUND_SAMPLE <<<"${output}")"

if [[ "${runs}" != 2 ]]; then
    printf 'expected all-reboot wrapper default SEMU_ACTOR_CROSS_REBOOT_RUNS=2, got %s\n' \
        "${runs}"
    exit 1
fi
if [[ "${executor}" != "threaded-cpu-with-device-actors" ]]; then
    printf 'expected all-reboot wrapper default EXECUTOR=threaded-cpu-with-device-actors, got %s\n' \
        "${executor}"
    exit 1
fi
if [[ "${smp}" != 2 ]]; then
    printf 'expected all-reboot wrapper default SMP=2, got %s\n' "${smp}"
    exit 1
fi
if [[ "${netdev}" != user ]]; then
    printf 'expected all-reboot wrapper default NETDEV=user, got %s\n' "${netdev}"
    exit 1
fi
if [[ -z "${timeout}" || -z "${cmd_timeout}" ]]; then
    printf '%s\n' 'expected all-reboot wrapper to print TIMEOUT and CMD_TIMEOUT'
    exit 1
fi
if [[ "${sound_sample}" != "/usr/share/sounds/alsa/Front_Center.wav" ]]; then
    printf 'expected default sound sample, got %s\n' "${sound_sample}"
    exit 1
fi

output="$(
    run_wrapper_config \
        SEMU_ACTOR_CROSS_REBOOT_RUNS=4 \
        EXECUTOR=custom-executor \
        SMP=3 \
        NETDEV=user \
        SEMU_ACTOR_CROSS_REBOOT_TIMEOUT=321 \
        SEMU_ACTOR_CROSS_REBOOT_CMD_TIMEOUT=123 \
        SEMU_ACTOR_CROSS_SOUND_SAMPLE=/tmp/sample.wav
)"
runs="$(get_value SEMU_ACTOR_CROSS_REBOOT_RUNS <<<"${output}")"
executor="$(get_value EXECUTOR <<<"${output}")"
smp="$(get_value SMP <<<"${output}")"
netdev="$(get_value NETDEV <<<"${output}")"
timeout="$(get_value TIMEOUT <<<"${output}")"
cmd_timeout="$(get_value SEMU_ACTOR_CROSS_REBOOT_CMD_TIMEOUT <<<"${output}")"
sound_sample="$(get_value SEMU_ACTOR_CROSS_SOUND_SAMPLE <<<"${output}")"

if [[ "${runs}" != 4 || "${executor}" != custom-executor || "${smp}" != 3 ||
      "${netdev}" != user || "${timeout}" != 321 || "${cmd_timeout}" != 123 ||
      "${sound_sample}" != /tmp/sample.wav ]]; then
    printf '%s\n' 'expected all-reboot wrapper to preserve caller overrides'
    printf '%s\n' "${output}"
    exit 1
fi

expect_invalid_runs 0
expect_invalid_runs ''
expect_invalid_runs abc

set +e
netdev_output="$(run_wrapper_config NETDEV=tap 2>&1)"
netdev_status=$?
set -e
if (( netdev_status == 0 )); then
    printf '%s\n' 'expected NETDEV=tap to fail'
    exit 1
fi
if ! grep -q 'all-reboot requires NETDEV=user' <<<"${netdev_output}"; then
    printf '%s\n' 'expected NETDEV=user validation message'
    exit 1
fi

output="$(run_actor_config SEMU_ACTOR_STRESS_TESTS=all-reboot)"
config_count="$(grep -c '^SEMU_ACTOR_CROSS_REBOOT_RUNS=' <<<"${output}")"
if [[ "${config_count}" != 1 ]]; then
    printf 'expected top-level all-reboot handoff to print one config, got %s\n' \
        "${config_count}"
    exit 1
fi
if grep -q '^=== actor stress run ' <<<"${output}"; then
    printf '%s\n' 'expected top-level all-reboot handoff to bypass outer actor loop'
    exit 1
fi

set +e
mixed_output="$(run_actor_config SEMU_ACTOR_STRESS_TESTS="gpu all-reboot" 2>&1)"
mixed_status=$?
set -e
if (( mixed_status == 0 )); then
    printf '%s\n' 'expected mixed all-reboot actor dispatcher input to fail'
    exit 1
fi
if ! grep -q 'all-reboot must be the only actor stress test' <<<"${mixed_output}"; then
    printf '%s\n' 'expected mixed all-reboot dispatcher failure message'
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
if ! grep -q 'all-reboot' <<<"${actor_output}"; then
    printf '%s\n' 'expected actor stress known-tests help to include all-reboot'
    exit 1
fi
