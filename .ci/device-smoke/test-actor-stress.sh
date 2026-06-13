#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

: "${SEMU_ACTOR_STRESS_RUNS=1}"
: "${SEMU_ACTOR_STRESS_TESTS=gpu vinput netdev}"

export EXECUTOR="${EXECUTOR:-threaded-cpu-with-device-actors}"
export SMP="${SMP:-2}"
export NETDEV="${NETDEV:-user}"

case "${SEMU_ACTOR_STRESS_RUNS}" in
    ''|*[!0-9]*)
        echo "FAIL: SEMU_ACTOR_STRESS_RUNS must be a positive integer" >&2
        exit 1
        ;;
esac
if (( 10#${SEMU_ACTOR_STRESS_RUNS} < 1 )); then
    echo "FAIL: SEMU_ACTOR_STRESS_RUNS must be a positive integer" >&2
    exit 1
fi

read -r -a ACTOR_STRESS_TESTS <<< "${SEMU_ACTOR_STRESS_TESTS}"
if (( ${#ACTOR_STRESS_TESTS[@]} == 0 )); then
    echo "FAIL: SEMU_ACTOR_STRESS_TESTS must name at least one test" >&2
    exit 1
fi

all_soak_count=0
for test_name in "${ACTOR_STRESS_TESTS[@]}"; do
    if [[ "${test_name}" == "all-soak" ]]; then
        (( all_soak_count += 1 ))
    fi
done
if (( all_soak_count > 0 )); then
    if (( ${#ACTOR_STRESS_TESTS[@]} == 1 )); then
        exec "${SCRIPT_DIR}/test-actor-stress-all-soak.sh" "$@"
    fi
    echo "FAIL: all-soak must be the only actor stress test" >&2
    exit 1
fi

all_reboot_count=0
for test_name in "${ACTOR_STRESS_TESTS[@]}"; do
    if [[ "${test_name}" == "all-reboot" ]]; then
        (( all_reboot_count += 1 ))
    fi
done
if (( all_reboot_count > 0 )); then
    if (( ${#ACTOR_STRESS_TESTS[@]} == 1 )); then
        exec "${SCRIPT_DIR}/test-actor-stress-all-reboot.sh" "$@"
    fi
    echo "FAIL: all-reboot must be the only actor stress test" >&2
    exit 1
fi

script_for_test() {
    case "$1" in
        gpu)
            printf '%s\n' "${SCRIPT_DIR}/test-gpu-actor.sh"
            ;;
        vinput)
            printf '%s\n' "${SCRIPT_DIR}/test-vinput-actor.sh"
            ;;
        netdev)
            printf '%s\n' "${SCRIPT_DIR}/test-netdev-actor.sh"
            ;;
        sound)
            printf '%s\n' "${SCRIPT_DIR}/test-sound-actor.sh"
            ;;
        gpu3d)
            printf '%s\n' "${SCRIPT_DIR}/test-gpu-3d.sh"
            ;;
        gpu3d-stress)
            printf '%s\n' "${SCRIPT_DIR}/test-gpu-3d-stress.sh"
            ;;
        gpu3d-reboot)
            printf '%s\n' "${SCRIPT_DIR}/test-gpu-3d-reboot-stress.sh"
            ;;
        gpu3d-window-close)
            printf '%s\n' "${SCRIPT_DIR}/test-gpu-3d-window-close-stress.sh"
            ;;
        gpu3d-reset-long)
            printf '%s\n' "${SCRIPT_DIR}/test-gpu-3d-reset-stress-long.sh"
            ;;
        gpu3d-soak)
            printf '%s\n' "${SCRIPT_DIR}/test-gpu-3d-soak.sh"
            ;;
        all-soak)
            printf '%s\n' "${SCRIPT_DIR}/test-actor-stress-all-soak.sh"
            ;;
        all-reboot)
            printf '%s\n' "${SCRIPT_DIR}/test-actor-stress-all-reboot.sh"
            ;;
        *)
            return 1
            ;;
    esac
}

for test_name in "${ACTOR_STRESS_TESTS[@]}"; do
    if ! script_for_test "${test_name}" >/dev/null; then
        echo "FAIL: unknown actor stress test '${test_name}'" >&2
        echo "Known tests: gpu vinput netdev sound gpu3d gpu3d-stress gpu3d-reboot gpu3d-window-close gpu3d-reset-long gpu3d-soak all-soak all-reboot" >&2
        exit 1
    fi
done

for (( run = 1; run <= 10#${SEMU_ACTOR_STRESS_RUNS}; run++ )); do
    for test_name in "${ACTOR_STRESS_TESTS[@]}"; do
        test_script="$(script_for_test "${test_name}")"

        printf '\n=== actor stress run %d/%d: %s ===\n' \
            "${run}" "${SEMU_ACTOR_STRESS_RUNS}" "${test_name}"
        printf 'EXECUTOR=%s SMP=%s NETDEV=%s\n' "${EXECUTOR}" "${SMP}" "${NETDEV}"
        printf 'script=%s\n' "${test_script}"

        "${test_script}"
    done
done
