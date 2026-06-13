#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

: "${SEMU_ACTOR_STRESS_RUNS=5}"
if [[ "${SEMU_ACTOR_STRESS_TESTS:-}" == "" ||
      "${SEMU_ACTOR_STRESS_TESTS}" == "all-soak" ]]; then
    SEMU_ACTOR_STRESS_TESTS="gpu vinput netdev sound"
elif [[ " ${SEMU_ACTOR_STRESS_TESTS} " == *" all-soak "* ]]; then
    echo "FAIL: all-soak must be the only actor stress test" >&2
    exit 1
fi

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

export SEMU_ACTOR_STRESS_RUNS SEMU_ACTOR_STRESS_TESTS

ACTOR_STRESS_SCRIPT="${SCRIPT_DIR}/test-actor-stress.sh"

if [[ "${SEMU_ACTOR_STRESS_SOAK_PRINT_CONFIG:-0}" == 1 ]]; then
    printf 'SEMU_ACTOR_STRESS_RUNS=%s\n' "${SEMU_ACTOR_STRESS_RUNS}"
    printf 'SEMU_ACTOR_STRESS_TESTS=%s\n' "${SEMU_ACTOR_STRESS_TESTS}"
    printf 'SEMU_ACTOR_STRESS_SCRIPT=%s\n' "${ACTOR_STRESS_SCRIPT}"
    exit 0
fi

exec "${ACTOR_STRESS_SCRIPT}" "$@"
