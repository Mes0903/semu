#!/usr/bin/env bash
#
# Shared source-build artifact contract. This file describes the artifact
# classes that the source builder can produce and the raw output files for each
# class. It does not describe CI recipe keys, stamps, resolver actions, or
# release publish policy.

set -euo pipefail

source_artifact_class_registry() {
    printf '%s\t%s\n' image Image
    printf '%s\t%s\n' rootfs rootfs.cpio
    printf '%s\t%s\n' test-tools test-tools.img
}

source_artifact_classes() {
    local class
    local _outputs

    while IFS=$'\t' read -r class _outputs; do
        printf '%s\n' "$class"
    done < <(source_artifact_class_registry)
}

source_artifact_class_outputs() {
    local want=$1
    local class
    local outputs

    while IFS=$'\t' read -r class outputs; do
        if [ "$class" = "$want" ]; then
            printf '%s\n' "$outputs" | tr ' ' '\n'
            return 0
        fi
    done < <(source_artifact_class_registry)

    echo "[!] Unknown source artifact class: $want" >&2
    return 1
}

source_artifact_outputs() {
    local class

    while IFS= read -r class; do
        source_artifact_class_outputs "$class"
    done < <(source_artifact_classes)
}

if [ "${BASH_SOURCE[0]}" = "$0" ]; then
    case "${1:-}" in
        outputs)
            source_artifact_outputs
            ;;
        output)
            [ "$#" -eq 2 ] || {
                echo "usage: $0 output <class>" >&2
                exit 2
            }
            source_artifact_class_outputs "$2"
            ;;
        *)
            echo "usage: $0 {outputs|output <class>}" >&2
            exit 2
            ;;
    esac
fi
