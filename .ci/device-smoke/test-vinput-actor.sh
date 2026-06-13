#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

export EXECUTOR="${EXECUTOR:-threaded-cpu-with-device-actors}"
export SMP="${SMP:-2}"
export NETDEV="${NETDEV:-user}"

exec "${SCRIPT_DIR}/test-vinput.sh"
