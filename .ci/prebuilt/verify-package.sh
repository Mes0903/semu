#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)

# Verification follows the class registry and checks package shape: each raw
# artifact has a compressed archive, prebuilt.sha1 has the expected archive and
# recipe-key entries, and archive checksum entries match the packaged bytes.
. "$SCRIPT_DIR/artifact-inputs.sh"

if [ ! -f prebuilt.sha1 ]; then
    echo "[!] Missing prebuilt.sha1" >&2
    exit 1
fi

manifest_entry_value() {
    local entry=$1

    awk -v f="$entry" '$2 == f {print $1; found = 1; exit} END {exit !found}' prebuilt.sha1
}

verify_archive_checksum_entry() {
    local archive=$1
    local expected
    local actual

    if ! expected=$(manifest_entry_value "$archive"); then
        echo "[!] prebuilt.sha1 missing $archive" >&2
        return 1
    fi

    actual=$(prebuilt_sha1sum "$archive" | awk '{print $1}')
    if [ "$actual" != "$expected" ]; then
        echo "[!] prebuilt.sha1 checksum mismatch for $archive" >&2
        echo "[!] expected $expected" >&2
        echo "[!] actual   $actual" >&2
        return 1
    fi
}

while IFS= read -r class; do
    while IFS= read -r artifact; do
        archive=${artifact}.bz2
        if [ ! -f "$archive" ]; then
            echo "[!] Missing $archive" >&2
            exit 1
        fi
        verify_archive_checksum_entry "$archive"
    done < <(source_artifact_class_outputs "$class")

    while IFS= read -r entry; do
        if ! manifest_entry_value "$entry" >/dev/null; then
            echo "[!] prebuilt.sha1 missing $entry" >&2
            exit 1
        fi
    done < <(prebuilt_class_recipe_entries "$class")
done < <(source_artifact_classes)
