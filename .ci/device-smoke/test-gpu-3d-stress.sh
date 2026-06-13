#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

: "${VGPU3D_GLXGEARS_RUNS:=3}"
: "${VGPU3D_XORG_RESTARTS:=1}"
export VGPU3D_GLXGEARS_RUNS
export VGPU3D_XORG_RESTARTS

exec "${SCRIPT_DIR}/test-gpu-3d.sh"
