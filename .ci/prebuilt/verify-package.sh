#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)

# Verification follows the class registry and checks package shape: each raw
# artifact has a compressed archive, and prebuilt.sha1 has the expected
# recipe-key entries. It deliberately does not validate archive byte checksums;
# prebuilt.sha1 is a recipe manifest, not a transport integrity manifest.
. "$SCRIPT_DIR/artifact-inputs.sh"

if [ ! -f prebuilt.sha1 ]; then
    echo "[!] Missing prebuilt.sha1" >&2
    exit 1
fi

while IFS= read -r class; do
    while IFS= read -r artifact; do
        archive=${artifact}.bz2
        if [ ! -f "$archive" ]; then
            echo "[!] Missing $archive" >&2
            exit 1
        fi
    done < <(source_artifact_class_outputs "$class")

    while IFS= read -r entry; do
        if ! awk -v f="$entry" '$2 == f {found = 1} END {exit !found}' prebuilt.sha1; then
            echo "[!] prebuilt.sha1 missing $entry" >&2
            exit 1
        fi
    done < <(prebuilt_class_recipe_entries "$class")
done < <(source_artifact_classes)
