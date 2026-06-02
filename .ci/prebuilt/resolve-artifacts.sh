#!/usr/bin/env bash
#
# CI planner for prebuilt guest artifacts. It never mutates raw artifacts; it
# compares the current recipe keys, the rolling-release recipe keys, and any
# restored CI-cache stamps for all registered classes, then prints a KEY=VALUE
# plan for the CI materializer. Local Make builds do not use this script.

set -euo pipefail

SCRIPT_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH='' cd -- "$SCRIPT_DIR/../.." && pwd)
cd "$REPO_ROOT"

# Load the shared artifact class contract plus CI recipe-key helpers. The build
# recipe itself lives under scripts/prebuilt; this file only describes
# cache/release keys and resolver decisions for CI.
. "$SCRIPT_DIR/artifact-inputs.sh"

# This is only for fork/mirror first-run bootstrap when their rolling prebuilt
# release has never existed. Other manifest fetch failures stay fatal.
case "${PREBUILT_BOOTSTRAP_ON_404:-0}" in
    1|true)
        bootstrap_on_404=true
        ;;
    0|false|'')
        bootstrap_on_404=false
        ;;
    *)
        echo "[!] PREBUILT_BOOTSTRAP_ON_404 must be 0/1 or true/false, got: ${PREBUILT_BOOTSTRAP_ON_404:-}" >&2
        exit 1
        ;;
esac

# Keep the resolver output comparable between jobs by always planning the
# full registered artifact set. Callers that only need one artifact should still
# consume the class they need from the complete plan.
if [ "$#" -ne 0 ]; then
    echo "[!] resolve-artifacts.sh does not accept class arguments; it resolves all artifact classes." >&2
    exit 1
fi

tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT

# The release manifest is downloaded into a temp directory so failed or
# partial network fetches never touch workspace artifacts.
manifest=$tmpdir/prebuilt.sha1
manifest_url=
release_manifest_http_code=

while IFS= read -r class; do
    prebuilt_plan_set_class_var "$class" release_recipe_key ''
done < <(source_artifact_classes)

# prebuilt.sha1 stores archive checksum entries plus recipe-key virtual entries
# such as Image.recipe-key. The resolver uses recipe keys for freshness decisions
# and requires archive checksum entries to be present; manifests that lack them
# are outside the current release contract and resolve as stale.
manifest_entry_value() {
    local entry=$1

    awk -v f="$entry" '$2 == f {print $1; found = 1; exit} END {exit !found}' "$manifest"
}

manifest_class_recipe_key() {
    local class=$1
    local artifact
    local next
    local recipe_key=
    local have_key=false

    while IFS= read -r artifact; do
        if ! manifest_entry_value "${artifact}.bz2" >/dev/null; then
            printf '\n'
            return
        fi
        if ! next=$(manifest_entry_value "${artifact}.recipe-key"); then
            printf '\n'
            return
        fi
        if [ "$have_key" = false ]; then
            recipe_key=$next
            have_key=true
        elif [ "$recipe_key" != "$next" ]; then
            printf '\n'
            return
        fi
    done < <(source_artifact_class_outputs "$class")

    printf '%s\n' "$recipe_key"
}

# Fetch the rolling release manifest and project it into per-class
# release_*_recipe_key fields. Missing per-class entries are not fatal here;
# they simply make that class fall through to a source build decision.
fetch_release_manifest() {
    local http_code
    local curl_err=$tmpdir/curl.err
    local class

    : "${PREBUILT_URL:?Need PREBUILT_URL pointing at the prebuilt artifact directory}"
    manifest_url="${PREBUILT_URL}/prebuilt.sha1"
    if http_code=$(curl --fail --silent --show-error --retry 3 --retry-delay 1 \
        --write-out '%{http_code}' -L -o "$manifest" "$manifest_url" 2>"$curl_err"); then
        release_manifest_http_code=$http_code
        while IFS= read -r class; do
            prebuilt_plan_set_class_var "$class" release_recipe_key "$(manifest_class_recipe_key "$class")"
        done < <(source_artifact_classes)
    else
        release_manifest_http_code=$http_code
        # Forks and mirrors may bootstrap only when the rolling release has not
        # been created yet. Network failures and other HTTP errors stay fatal so
        # external release or network problems remain visible.
        if [ "$bootstrap_on_404" = false ] || [ "$release_manifest_http_code" != 404 ]; then
            cat "$curl_err" >&2
        fi
        echo "prebuilt manifest is unavailable at $manifest_url" >&2
        return 1
    fi
}

class_any_artifact_exists() {
    local artifact

    while IFS= read -r artifact; do
        if [ -f "$artifact" ]; then
            return 0
        fi
    done < <(source_artifact_class_outputs "$1")

    return 1
}

# Read recipe-key stamps from a restored GitHub Actions raw-artifact cache.
# A class may contain multiple raw artifact files, so every file must exist,
# have a stamp, and agree on the same recipe key before the cache is trusted.
class_local_recipe_key() {
    local class=$1
    local artifact
    local recipe_stamp
    local recipe_key=
    local next=
    local have_key=false

    while IFS= read -r artifact; do
        recipe_stamp=$(prebuilt_artifact_recipe_stamp "$artifact")
        if [ ! -f "$artifact" ] || [ ! -f "$recipe_stamp" ]; then
            return 1
        fi
        IFS= read -r next < "$recipe_stamp" || return 1
        if [ -z "$next" ]; then
            return 1
        fi
        if [ "$have_key" = false ]; then
            recipe_key=$next
            have_key=true
        elif [ "$recipe_key" != "$next" ]; then
            return 1
        fi
    done < <(source_artifact_class_outputs "$class")

    if [ "$have_key" = false ]; then
        return 1
    fi

    printf '%s\n' "$recipe_key"
}

# Local state here means a CI cache restore, not a developer workspace.
# CI trusts a matching recipe-key stamp as the cache contract; raw artifact
# content corruption is intentionally left as a normal CI failure to inspect.
# Status values are: missing = no raw artifact, valid = artifact plus matching
# stamps, unmanaged = raw artifact exists but cannot be tied to a recipe key.
resolve_local_class() {
    local class=$1
    local recipe_key=
    local status=missing

    if class_any_artifact_exists "$class"; then
        if recipe_key=$(class_local_recipe_key "$class" 2>/dev/null); then
            status=valid
        else
            status=unmanaged
        fi
    fi

    prebuilt_plan_set_class_var "$class" local_recipe_key "$recipe_key"
    prebuilt_plan_set_class_var "$class" local_status "$status"
}

# The action is the only authority consumed by materialize-artifacts.sh:
# use-action-cache, download-release, or build. Prefer already-restored raw
# artifacts first, then the rolling release, and build from source only when
# neither source matches the current checkout recipe.
decide_action() {
    local current_recipe_key=$1
    local release_recipe_key=$2
    local local_recipe_key=$3
    local local_status=$4

    if [ "$local_status" = valid ] && [ "$local_recipe_key" = "$current_recipe_key" ]; then
        printf '%s\n' use-action-cache
        return
    fi

    if [ -n "$release_recipe_key" ] && [ "$release_recipe_key" = "$current_recipe_key" ]; then
        printf '%s\n' download-release
    else
        printf '%s\n' build
    fi
}

# Collapse the current per-class recipe keys into a single cache namespace
# for the workflow-local raw artifact bundle. This is not an artifact checksum;
# materialization correctness still comes from each class action and stamp.
compute_platform_action_cache_tag() {
    local class

    while IFS= read -r class; do
        prebuilt_plan_get_class_var "$class" current_recipe_key
    done < <(source_artifact_classes) | prebuilt_sha1sum | awk '{print $1}'
}

# Compute checkout-derived recipe keys before consulting the release. These
# keys describe the artifacts this workflow wants, independent of what exists
# in cache or in the rolling release.
while IFS= read -r class; do
    prebuilt_plan_set_class_var "$class" current_recipe_key "$(prebuilt_class_recipe_key "$class")"
    resolve_local_class "$class"
done < <(source_artifact_classes)
platform_action_cache_tag=$(compute_platform_action_cache_tag)
ci_cache_schema_tag=$(prebuilt_ci_cache_schema_tag)

# A missing manifest is only tolerated for fork/mirror bootstrap. In that
# case release_*_recipe_key fields stay empty, so every class that is not
# satisfied by restored cache will choose build.
if ! fetch_release_manifest; then
    if [ "$bootstrap_on_404" = true ] && [ "$release_manifest_http_code" = 404 ]; then
        echo "prebuilt manifest is missing; bootstrapping from source" >&2
    else
        exit 1
    fi
fi

# Convert the three observed states for each class into the materializer
# action that will be executed later in the workflow. Workflow plumbing uses
# requires_build to decide whether build dependencies are needed before
# materialization.
requires_build=false
while IFS= read -r class; do
    action=$(decide_action \
        "$(prebuilt_plan_get_class_var "$class" current_recipe_key)" \
        "$(prebuilt_plan_get_class_var "$class" release_recipe_key)" \
        "$(prebuilt_plan_get_class_var "$class" local_recipe_key)" \
        "$(prebuilt_plan_get_class_var "$class" local_status)")
    prebuilt_plan_set_class_var "$class" action "$action"
    if [ "$action" = build ]; then
        requires_build=true
    fi
done < <(source_artifact_classes)

# Publishing cares about the rolling release as a whole. If any class in the
# release manifest does not match the current recipe, the release needs update.
release_needs_update=false
while IFS= read -r class; do
    if [ "$(prebuilt_plan_get_class_var "$class" release_recipe_key)" != "$(prebuilt_plan_get_class_var "$class" current_recipe_key)" ]; then
        release_needs_update=true
        break
    fi
done < <(source_artifact_classes)

# Print a shell-friendly plan. materialize-artifacts.sh consumes the action and
# current recipe-key fields; the remaining scalar fields feed workflow
# conditions, cache keys, and publish gates.
printf 'plan_version=1\n'
prebuilt_plan_print_class_values action
prebuilt_plan_print_class_values current_recipe_key
printf 'platform_action_cache_tag=%s\n' "$platform_action_cache_tag"
printf 'ci_cache_schema_tag=%s\n' "$ci_cache_schema_tag"
printf 'requires_build=%s\n' "$requires_build"
printf 'release_needs_update=%s\n' "$release_needs_update"
