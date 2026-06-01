#!/usr/bin/env bash
#
# CI executor for a resolver plan. It deliberately does not run the resolver or
# accept class arguments: the resolver plans all registered classes, then callers
# pass PREBUILT_PLAN_FILE here. Local Make builds bypass this layer entirely.

set -euo pipefail

SCRIPT_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH='' cd -- "$SCRIPT_DIR/../.." && pwd)
cd "$REPO_ROOT"

# Load the class/plan registry and stamp writer as the materializer's shared
# contract with the resolver.  The materializer consumes plan keys from the
# registry and commits stamps only after download/build actions succeed.
{
    . "$SCRIPT_DIR/artifact-inputs.sh"
    . "$SCRIPT_DIR/stamp-artifacts.sh"
}

if [ "$#" -ne 0 ]; then
    echo "[!] materialize-artifacts.sh does not accept class arguments; pass them when creating the plan." >&2
    exit 1
fi

# Downloading consumes an already-decided release action. After every raw
# artifact in the class lands, the materializer commits the recipe-key stamp so
# later CI cache restores can be trusted as use-action-cache actions. Release
# archive downloads are transport, not recipe verification; the resolver has
# already checked the release recipe key against current source state.
download_release_class() {
    local class=$1
    local recipe_key=$2
    local artifact

    while IFS= read -r artifact; do
        scripts/prebuilt/download-release-artifact.sh "$artifact" || {
            echo "[!] Failed to materialize $class from release" >&2
            exit 1
        }
    done < <(source_artifact_class_outputs "$class")
    prebuilt_stamp_artifacts_for_class "$class" "$recipe_key"
}

# Build actions go through the internal CI builder, not scripts/build-artifacts.sh.
# build-artifacts.sh is the user-facing local CLI; the materializer stamps only
# after the plan build action succeeds.
build_plan_classes() {
    local -a args=()
    local class

    while IFS= read -r class; do
        if [ "$(prebuilt_plan_get_class_var "$class" action)" = build ]; then
            args+=("$class")
        fi
    done < <(source_artifact_classes)

    if [ "${#args[@]}" -eq 0 ]; then
        return 0
    fi

    .ci/prebuilt/build-plan-artifacts.sh "${args[@]}" >&2
}

# Materialization consumes plan_version, fixed workflow scalars, *_action, and
# current_*_recipe_key. Unknown future keys fail fast so the resolver/materializer
# plan schema cannot silently drift.
parse_resolver_output() {
    local line
    local plan_key
    local value
    local class
    local matched

    while IFS= read -r line; do
        plan_key=${line%%=*}
        value=${line#*=}
        case "$plan_key" in
            plan_version) plan_version=$value; continue ;;
            requires_build)
                continue
                ;;
            platform_action_cache_tag|\
            ci_cache_schema_tag|\
            release_manifest_available|\
            release_needs_update)
                continue
                ;;
        esac

        matched=false
        while IFS= read -r class; do
            if prebuilt_plan_class_var_matches "$plan_key" "$class" action current_recipe_key; then
                printf -v "$plan_key" '%s' "$value"
                matched=true
                break
            fi
        done < <(source_artifact_classes)

        if [ "$matched" = true ]; then
            continue
        fi

        echo "[!] Unknown prebuilt plan key: $plan_key" >&2
        return 1
    done
}

# The resolver is intentionally a separate step. Requiring an explicit
# plan file makes materialization reproducible and lets workflow steps pass the
# same plan through cache restore, build, and test jobs.
if [ -z "${PREBUILT_PLAN_FILE:-}" ]; then
    echo "[!] PREBUILT_PLAN_FILE is required; create a CI resolver plan before materializing." >&2
    exit 1
fi
if [ ! -f "$PREBUILT_PLAN_FILE" ]; then
    echo "[!] PREBUILT_PLAN_FILE does not exist: $PREBUILT_PLAN_FILE" >&2
    exit 1
fi
resolver_output=$(cat "$PREBUILT_PLAN_FILE")

# Test jobs use this guard when they are supposed to consume only existing
# workflow artifacts or the rolling release. A build action in that context is
# a planner/publish-state problem, not something the test job should repair.
case "${PREBUILT_FORBID_BUILD:-0}" in
    1|true)
        forbid_build=true
        ;;
    0|false|'')
        forbid_build=false
        ;;
    *)
        echo "[!] PREBUILT_FORBID_BUILD must be 0/1 or true/false, got: ${PREBUILT_FORBID_BUILD:-}" >&2
        exit 1
        ;;
esac

# Initialize the class-scoped fields that are required for execution.
# parse_resolver_output fills them from the plan and leaves missing values empty
# so the validation below can report a precise schema error.
plan_version=
while IFS= read -r class; do
    prebuilt_plan_set_class_var "$class" action ''
    prebuilt_plan_set_class_var "$class" current_recipe_key ''
done < <(source_artifact_classes)

parse_resolver_output <<< "$resolver_output"

# Version the plan format even though it is just KEY=VALUE text. That lets a
# future resolver change fail loudly instead of being interpreted by an older
# materializer with subtly different semantics.
if [ "$plan_version" != 1 ]; then
    echo "[!] Unsupported prebuilt plan version: ${plan_version:-missing}" >&2
    exit 1
fi

# Validate every class before doing any work. This keeps a malformed plan
# from partially downloading or building one class before failing on another.
while IFS= read -r class; do
    action=$(prebuilt_plan_get_class_var "$class" action)
    case "$action" in
        build|use-action-cache|download-release)
            ;;
        '')
            echo "[!] Missing action for prebuilt class: $class" >&2
            exit 1
            ;;
        *)
            echo "[!] Unsupported action for prebuilt class $class: $action" >&2
            exit 1
            ;;
    esac

    case "$action" in
        build|download-release)
            if [ -z "$(prebuilt_plan_get_class_var "$class" current_recipe_key)" ]; then
                echo "[!] Missing current recipe key for prebuilt class $class with action $action" >&2
                exit 1
            fi
            ;;
    esac
done < <(source_artifact_classes)

# Collapse per-class actions into a single branch for dependency setup and
# PREBUILT_FORBID_BUILD enforcement. use-action-cache and download-release do
# not need source-build dependencies.
requires_build=false
while IFS= read -r class; do
    case "$(prebuilt_plan_get_class_var "$class" action)" in
        build) requires_build=true ;;
    esac
done < <(source_artifact_classes)

# When source builds are forbidden, report the exact class actions that
# violated the contract so the workflow log points back to the resolver state.
if [ "$requires_build" = true ] && [ "$forbid_build" = true ]; then
    echo "[!] PREBUILT_FORBID_BUILD forbids source builds, but the plan contains:" >&2
    while IFS= read -r class; do
        if [ "$(prebuilt_plan_get_class_var "$class" action)" = build ]; then
            printf '[!]   %s=build\n' "$(prebuilt_plan_class_var_name "$class" action)" >&2
        fi
    done < <(source_artifact_classes)
    exit 1
fi

# Build all planned source-build classes first. The internal builder only
# creates raw artifacts; stamps are written here after the whole build action
# succeeds so future cache restores do not trust half-built outputs.
if [ "$requires_build" = true ]; then
    build_plan_classes
    while IFS= read -r class; do
        if [ "$(prebuilt_plan_get_class_var "$class" action)" = build ]; then
            prebuilt_stamp_artifacts_for_class "$class" "$(prebuilt_plan_get_class_var "$class" current_recipe_key)"
        fi
    done < <(source_artifact_classes)
fi

# Finally materialize release-backed classes. use-action-cache classes need
# no work here because the workflow cache restore already placed both raw
# artifacts and .stamps/prebuilt-local stamps in the workspace.
while IFS= read -r class; do
    if [ "$(prebuilt_plan_get_class_var "$class" action)" = download-release ]; then
        download_release_class "$class" "$(prebuilt_plan_get_class_var "$class" current_recipe_key)"
    fi
done < <(source_artifact_classes)
