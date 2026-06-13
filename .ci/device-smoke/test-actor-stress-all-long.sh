#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

: "${SEMU_ACTOR_STRESS_RUNS=3}"
: "${SEMU_ACTOR_STRESS_TESTS=gpu vinput netdev sound}"
export SEMU_ACTOR_STRESS_RUNS SEMU_ACTOR_STRESS_TESTS

exec "${SCRIPT_DIR}/test-actor-stress.sh" "$@"
