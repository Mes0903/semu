#!/usr/bin/env bash

# Helper library for the CI prebuilt flow. Resolver, materializer, packager,
# stamp helper, and tests source this file to share CI recipe-key helpers and
# resolver plan naming. Local default make flows do not use these helpers.
#
# Stable artifact class identity and raw output filenames live in
# scripts/prebuilt/artifact-classes.sh because they are source-build contract,
# not CI policy. This file adapts that contract for CI recipe keys and plans.
#
# File recipe inputs stay in prebuilt_class_inputs() because they are CI
# cache-key metadata for the shared source-build API and are easier to review as
# explicit per-class lists. For variables in
# scripts/prebuilt/artifact-recipe.env, each class hashes only the KEY=VALUE
# lines that affect it. That keeps CI invalidation per-class without splitting
# the local build recipe into multiple env files.

# New artifact classes are declared in scripts/prebuilt/artifact-classes.sh and
# built in scripts/prebuilt/artifact-recipes.sh. This file only adds the CI
# recipe-key inputs/env vars for those shared classes.
. "$(CDPATH='' cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../scripts/prebuilt" && pwd)/artifact-classes.sh"

# test-tools recipe selection is build behavior first and CI key metadata
# second. Source the build-owned helper so local builds and CI keys cannot drift.
. "$(CDPATH='' cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../scripts/prebuilt" && pwd)/test-tools-recipe.sh"

# List real files that affect one class recipe key. Recipe variables from
# scripts/prebuilt/artifact-recipe.env are handled separately by
# prebuilt_class_recipe_env_vars(). Keep this synchronized with
# scripts/prebuilt/artifact-recipes.sh: if a source-build recipe starts reading
# a new file, add it here or CI can silently reuse a stale raw artifact.
prebuilt_class_inputs() {
    case "$1" in
        image)
            printf '%s\n' \
                configs/buildroot.config \
                configs/linux.config \
                scripts/prebuilt/artifact-recipes.sh
            ;;
        rootfs)
            printf '%s\n' \
                configs/buildroot.config \
                configs/busybox.config \
                scripts/prebuilt/artifact-recipes.sh \
                target/init
            ;;
        test-tools)
            prebuilt_test_tools_recipe_key >/dev/null || return 1
            printf '%s\n' \
                configs/buildroot.config \
                configs/busybox.config \
                scripts/prebuilt/artifact-recipes.sh \
                scripts/prebuilt/test-tools-recipe.sh \
                scripts/rootfs_ext4.sh \
                target/init
            if prebuilt_test_tools_recipe_includes x11; then
                printf '%s\n' configs/x11.config
            fi
            if prebuilt_test_tools_recipe_includes directfb2; then
                printf '%s\n' \
                    configs/meson-riscv-cross-file \
                    target/local-env.sh
            fi
            ;;
        *)
            echo "[!] Unknown prebuilt input class: $1" >&2
            return 1
            ;;
    esac
}

# List the de-duplicated real-file inputs across all classes.  Use this when
# a caller needs a whole-system source-input set, such as package validation or
# test fixture setup.
prebuilt_inputs() {
    local class

    while IFS= read -r class; do
        prebuilt_class_inputs "$class"
    done < <(source_artifact_classes) | awk '!seen[$0]++'
}

# Resolver plans are shell-friendly KEY=VALUE files. Class-scoped keys are
# derived from the class name, using underscores where shell variable names
# cannot use dashes. The resolver emits action and current_recipe_key fields;
# release/local fields are internal CI decision state.
prebuilt_plan_class_var_name() {
    local class=$1
    local kind=$2
    local prefix

    source_artifact_class_outputs "$class" >/dev/null || return 1
    prefix=${class//-/_}
    case "$kind" in
        action) printf '%s_action\n' "$prefix" ;;
        current_recipe_key) printf 'current_%s_recipe_key\n' "$prefix" ;;
        release_recipe_key) printf 'release_%s_recipe_key\n' "$prefix" ;;
        local_recipe_key) printf 'local_%s_recipe_key\n' "$prefix" ;;
        local_status) printf 'local_%s_status\n' "$prefix" ;;
        *) echo "[!] Unknown prebuilt plan variable kind: $kind" >&2; return 1 ;;
    esac
}

# Set one generated class-scoped plan variable.  Use this instead of spelling
# out image_action/rootfs_action/test_tools_action style names at each caller.
prebuilt_plan_set_class_var() {
    local class=$1
    local kind=$2
    local value=$3
    local var

    var=$(prebuilt_plan_class_var_name "$class" "$kind") || return 1
    printf -v "$var" '%s' "$value"
}

# Read one generated class-scoped plan variable.  Missing variables read as an
# empty value so parsers can validate required fields explicitly.
prebuilt_plan_get_class_var() {
    local class=$1
    local kind=$2
    local var

    var=$(prebuilt_plan_class_var_name "$class" "$kind") || return 1
    printf '%s\n' "${!var-}"
}

# Print one plan field for every class as KEY=VALUE lines.  Resolver output
# uses this to stay machine-readable without hard-coded class names.
prebuilt_plan_print_class_values() {
    local kind=$1
    local class

    while IFS= read -r class; do
        printf '%s=%s\n' \
            "$(prebuilt_plan_class_var_name "$class" "$kind")" \
            "$(prebuilt_plan_get_class_var "$class" "$kind")"
    done < <(source_artifact_classes)
}

# Test whether a plan key is one of the generated variable names for a class.
# Plan parsers use this to recognize class-scoped fields without duplicating the
# naming rules.
prebuilt_plan_class_var_matches() {
    local plan_key=$1
    local class=$2
    local kind
    shift 2

    for kind in "$@"; do
        if [ "$plan_key" = "$(prebuilt_plan_class_var_name "$class" "$kind")" ]; then
            return 0
        fi
    done

    return 1
}

# List prebuilt.sha1 recipe-key entries for all artifacts in one class.
prebuilt_class_recipe_entries() {
    local artifact

    while IFS= read -r artifact; do
        printf '%s.recipe-key\n' "$artifact"
    done < <(source_artifact_class_outputs "$1")
}

# Local recipe-key stamps live with other generated build metadata. The raw
# artifacts stay at the repository root; this directory stores only metadata
# used to decide whether restored local artifacts match the current recipe.
prebuilt_local_stamp_dir=.stamps/prebuilt-local

# Map a raw artifact filename to the CI cache stamp that records its recipe key.
prebuilt_artifact_recipe_stamp() {
    printf '%s/%s.recipe-key\n' "$prebuilt_local_stamp_dir" "$1"
}

# Recipe keys are produced on Linux and consumed again on Linux/macOS test
# jobs. Keep this helper limited to sha1sum-compatible output over explicit
# relative paths so GNU sha1sum and macOS shasum generate identical manifests.
prebuilt_sha1sum() {
    if command -v sha1sum >/dev/null 2>&1; then
        sha1sum "$@"
    elif command -v shasum >/dev/null 2>&1; then
        shasum -a 1 "$@"
    else
        echo "[!] Need sha1sum or shasum on PATH" >&2
        return 1
    fi
}

# CI raw artifact cache keys combine artifact recipe keys with this schema tag.
# Recipe keys cover artifact bytes; these files cover CI planning and
# materialization behavior that can change how restored raw artifacts are
# interpreted.
prebuilt_ci_cache_schema_inputs() {
    printf '%s\n' \
        .ci/prebuilt/artifact-inputs.sh \
        .ci/prebuilt/build-plan-artifacts.sh \
        .ci/prebuilt/resolve-artifacts.sh \
        .ci/prebuilt/stamp-artifacts.sh \
        .ci/prebuilt/materialize-artifacts.sh
}

prebuilt_ci_cache_schema_tag() {
    local -a inputs=()
    local input

    while IFS= read -r input; do
        inputs+=("$input")
    done < <(prebuilt_ci_cache_schema_inputs)

    for input in "${inputs[@]}"; do
        if [ ! -f "$input" ]; then
            echo "[!] Missing prebuilt CI cache schema input $input" >&2
            return 1
        fi
    done

    prebuilt_sha1sum "${inputs[@]}" | prebuilt_sha1sum | awk '{print $1}'
}

# Return the shared local build recipe env file path. CI hashes selected lines
# from this file, but the local build keeps one recipe file.
prebuilt_recipe_env_file() {
    printf '%s\n' scripts/prebuilt/artifact-recipe.env
}

# Read exactly one KEY=VALUE line from the recipe env file.  Missing or
# duplicate variables are errors because recipe keys must be deterministic.
prebuilt_recipe_env_line() {
    local name=$1
    local env_file
    local line

    env_file=$(prebuilt_recipe_env_file)
    if [ ! -f "$env_file" ]; then
        echo "[!] Missing prebuilt recipe env $env_file" >&2
        return 1
    fi
    line=$(grep -E "^${name}=" "$env_file" || true)
    if [ -z "$line" ]; then
        echo "[!] Missing prebuilt recipe variable $name in $env_file" >&2
        return 1
    fi
    if [ "$(printf '%s\n' "$line" | wc -l | tr -d ' ')" != 1 ]; then
        echo "[!] Duplicate prebuilt recipe variable $name in $env_file" >&2
        return 1
    fi
    printf '%s\n' "$line"
}

# List recipe env variables that affect one class. Keep this synchronized with
# artifact-recipes.sh and artifact-recipe.env; missed variables make CI
# invalidation stale even though local source builds still see the new value.
prebuilt_class_recipe_env_vars() {
    local class=$1

    case "$class" in
        image)
            printf '%s\n' \
                BUILDROOT_REPO \
                BUILDROOT_REV \
                LINUX_REPO \
                LINUX_REV
            ;;
        rootfs)
            printf '%s\n' \
                BUILDROOT_REPO \
                BUILDROOT_REV
            ;;
        test-tools)
            printf '%s\n' \
                BUILDROOT_REPO \
                BUILDROOT_REV \
                TEST_TOOLS_SIZE_MB
            if prebuilt_test_tools_recipe_includes directfb2; then
                printf '%s\n' \
                    DIRECTFB2_REPO \
                    DIRECTFB2_REV \
                    DIRECTFB_EXAMPLES_REPO \
                    DIRECTFB_EXAMPLES_REV
            fi
            ;;
        *)
            echo "[!] Unknown prebuilt input class: $class" >&2
            return 1
            ;;
    esac
}

# Hash one recipe env KEY=VALUE line and label it in the class recipe manifest.
# The label records which recipe variable affected the key.
prebuilt_recipe_env_virtual_entry() {
    local name=$1
    local line

    line=$(prebuilt_recipe_env_line "$name") || return 1
    printf '%s\n' "$line" | prebuilt_sha1sum | \
        awk -v name="$name" '{print $1 "  artifact-recipe.env:" name}'
}

# Emit non-file recipe inputs for one class, such as selected recipe env lines
# and the test-tools recipe selection.
prebuilt_class_recipe_virtual_entries() {
    local class=$1
    local value
    local recipe_var

    while IFS= read -r recipe_var; do
        prebuilt_recipe_env_virtual_entry "$recipe_var"
    done < <(prebuilt_class_recipe_env_vars "$class")

    case "$class" in
        test-tools)
            value=$(prebuilt_test_tools_recipe_key) || return 1
            printf '%s' "$value" | prebuilt_sha1sum | \
                awk '{print $1 "  test-tools.recipe"}'
            ;;
    esac
}

# Build the full recipe manifest for one class.  It contains hashes for real
# input files plus the non-file inputs that also affect the artifact bytes.
prebuilt_class_recipe_manifest() {
    local class=$1
    local -a inputs=()
    local input

    while IFS= read -r input; do
        inputs+=("$input")
    done < <(prebuilt_class_inputs "$class")

    if [ "${#inputs[@]}" -eq 0 ]; then
        echo "[!] Prebuilt input class $class has no inputs" >&2
        return 1
    fi

    for input in "${inputs[@]}"; do
        if [ ! -f "$input" ]; then
            echo "[!] Missing prebuilt input $input" >&2
            return 1
        fi
    done

    prebuilt_sha1sum "${inputs[@]}"
    prebuilt_class_recipe_virtual_entries "$class"
}

# Collapse one class recipe manifest into the single SHA-1 key used by resolver
# decisions, release manifests, and CI cache stamps.
prebuilt_class_recipe_key() {
    local manifest

    manifest=$(prebuilt_class_recipe_manifest "$1") || return 1
    printf '%s\n' "$manifest" | prebuilt_sha1sum | awk '{print $1}'
}
