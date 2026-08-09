#!/usr/bin/env bash
#
# CI stamp helper. A stamp records which recipe key produced an existing raw
# artifact restored from CI cache, downloaded from release, or built in CI.
# It is not an archive checksum and is not a publication manifest.

set -euo pipefail

SCRIPT_DIR=$(CDPATH='' cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)

# Stamping needs only the class registry and recipe-key helpers. The CI
# materializer sources this file and calls the stamp functions directly.
# shellcheck source=.ci/prebuilt/artifact-inputs.sh
. "$SCRIPT_DIR/artifact-inputs.sh"

prebuilt_stamp_artifacts_for_class() {
    local class=$1
    local recipe_key=$2
    local artifact

    if [ -z "$recipe_key" ]; then
        echo "[!] Cannot stamp $class with an empty recipe key" >&2
        return 1
    fi

    mkdir -p "$prebuilt_local_stamp_dir" || return 1
    while IFS= read -r artifact; do
        if [ ! -f "$artifact" ]; then
            echo "[!] Cannot stamp missing artifact: $artifact" >&2
            return 1
        fi
        printf '%s\n' "$recipe_key" > "$(prebuilt_artifact_recipe_stamp "$artifact")"
    done < <(source_artifact_class_outputs "$class")
}
