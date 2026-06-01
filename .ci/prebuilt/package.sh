#!/usr/bin/env bash
#
# Compress raw prebuilt guest artifacts and write prebuilt.sha1.
# This script does not publish anything to a remote service.  The manifest
# contains recipe-key virtual entries only.  Resolver/materializer decisions are
# based on recipe state, not archive byte checksums.
#
# Inputs (in cwd):
#   raw guest artifacts listed by .ci/prebuilt/artifact-inputs.sh
#   plus the source inputs listed by .ci/prebuilt/artifact-inputs.sh
#
# Outputs (in cwd):
#   <artifact>.bz2 for each raw guest artifact
#   prebuilt.sha1   -- recipe manifest in sha1sum format. *.recipe-key
#                      lines record the source recipe fingerprint for each
#                      artifact class.
#
set -euo pipefail

SCRIPT_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)

# Package layout is derived from the same class registry as planning and
# materialization, so the release manifest follows class additions without a
# second hard-coded artifact list in this script.
. "$SCRIPT_DIR/artifact-inputs.sh"

require_raw_artifacts() {
    local class
    local artifact

    while IFS= read -r class; do
        while IFS= read -r artifact; do
            if [ ! -f "$artifact" ]; then
                echo "[!] Missing $artifact -- materialize it in the CI guest-artifact-build job first" >&2
                exit 1
            fi
        done < <(source_artifact_class_outputs "$class")
    done < <(source_artifact_classes)
}

require_source_inputs() {
    local -a inputs=()
    local input

    while IFS= read -r input; do
        inputs+=("$input")
    done < <(prebuilt_inputs)

    for input in "${inputs[@]}"; do
        if [ ! -f "$input" ]; then
            echo "[!] Missing prebuilt source input $input -- check repository checkout and artifact input lists" >&2
            exit 1
        fi
    done
}

tmp_archives=()
cleanup_package_parts() {
    if [ "${#tmp_archives[@]}" -gt 0 ]; then
        rm -f "${tmp_archives[@]}"
    fi
}
trap cleanup_package_parts EXIT

compress_artifact() {
    local artifact=$1
    local archive=${artifact}.bz2
    local part=${archive}.part

    tmp_archives+=("$part")
    if ! bzip2 -c "$artifact" > "$part"; then
        rm -f "$part"
        return 1
    fi
    mv -f "$part" "$archive"
}

compress_class_artifacts() {
    local class=$1
    local artifact

    while IFS= read -r artifact; do
        compress_artifact "$artifact"
    done < <(source_artifact_class_outputs "$class")
}

write_manifest_entries_for_class() {
    local class=$1
    local recipe_key=$2
    local entry

    while IFS= read -r entry; do
        printf '%s  %s\n' "$recipe_key" "$entry"
    done < <(prebuilt_class_recipe_entries "$class")
}

require_raw_artifacts
require_source_inputs

# Package raw artifacts first, then write the recipe-key manifest from the same
# class registry so archive names and manifest entries stay aligned.
while IFS= read -r class; do
    compress_class_artifacts "$class"
done < <(source_artifact_classes)

{
    while IFS= read -r class; do
        write_manifest_entries_for_class "$class" "$(prebuilt_class_recipe_key "$class")"
    done < <(source_artifact_classes)
} > prebuilt.sha1
