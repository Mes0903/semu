#!/usr/bin/env bash

set -euo pipefail

# Fast build-system contract tests.
#
# Keep this script cheap and deterministic. It should not download guest
# artifacts, unpack release assets, or trigger a real Buildroot/Linux build.
# Slower CI jobs cover those paths. The purpose here is to pin the shell
# contracts that glue those jobs together: artifact class registration, recipe
# inputs, local CLI validation, Make download recipes, materializer plan
# parsing, and the Buildroot source-build lock.
#
# Generated files live under TEST_TMP so the tests can freely create fake plans,
# lock paths, and temporary Makefiles without touching real artifacts in the
# checkout.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

cd "$REPO_ROOT"

# All fixtures are disposable. Logs are kept under the same directory so failure
# paths can dump useful context without leaving state in the repository.
TEST_TMP=$(mktemp -d "${TMPDIR:-/tmp}/semu-build-system-test.XXXXXX")
cleanup() {
    rm -rf "$TEST_TMP"
}
trap cleanup EXIT

# Assertion helpers print enough context to identify the broken contract from CI
# logs. Keep them small and POSIX-friendly because this script is run by macOS
# and Linux jobs.
fail() {
    printf '[!] %s\n' "$*" >&2
    exit 1
}

assert_eq() {
    local actual=$1
    local expected=$2
    local label=$3

    if [ "$actual" != "$expected" ]; then
        printf '[!] %s\n' "$label" >&2
        printf '[!] expected:\n%s\n' "$expected" >&2
        printf '[!] actual:\n%s\n' "$actual" >&2
        exit 1
    fi
}

assert_contains() {
    local haystack=$1
    local needle=$2
    local label=$3

    case "$haystack" in
        *"$needle"*) ;;
        *) fail "$label: missing '$needle'" ;;
    esac
}

assert_not_contains() {
    local haystack=$1
    local needle=$2
    local label=$3

    case "$haystack" in
        *"$needle"*) fail "$label: unexpected '$needle'" ;;
    esac
}

# Each check is a named function so CI output points at the failed build-system
# contract instead of only reporting a line number inside this driver.
run_test() {
    local name=$1
    shift

    printf '  TEST\t%s\n' "$name"
    "$@"
}

# Diagnostic text is part of the contract. Capture stdout/stderr into a log so
# callers can assert that failures are actionable, not just non-zero.
expect_failure() {
    local label=$1
    local log=$2
    shift 2

    if "$@" >"$log" 2>&1; then
        cat "$log" >&2
        fail "$label: command unexpectedly succeeded"
    fi
}

# Success checks use the same log convention; on failure the captured output is
# printed before the test exits.
expect_success() {
    local label=$1
    local log=$2
    shift 2

    if ! "$@" >"$log" 2>&1; then
        cat "$log" >&2
        fail "$label: command failed"
    fi
}

# Source pure helper libraries in the parent for unit-style checks. The lock
# helper is sourced later because it installs traps only after acquiring a lock.
. scripts/prebuilt/artifact-classes.sh
. scripts/prebuilt/test-tools-recipe.sh
. .ci/prebuilt/artifact-inputs.sh

# The artifact class registry is shared by source builds, release downloads,
# action-cache planning, and Make targets. A silent class/output rename would
# desynchronize those paths, so keep the public class contract pinned here.
test_artifact_class_contract() {
    local log="$TEST_TMP/unknown-artifact-class.log"

    assert_eq "$(source_artifact_classes)" $'image\nrootfs\ntest-tools' \
        "artifact class list changed unexpectedly"
    assert_eq "$(source_artifact_outputs)" $'Image\nrootfs.cpio\ntest-tools.img' \
        "artifact output list changed unexpectedly"
    assert_eq "$(source_artifact_class_outputs test-tools)" "test-tools.img" \
        "test-tools class output changed unexpectedly"

    expect_failure "unknown artifact class" "$log" \
        scripts/prebuilt/artifact-classes.sh output does-not-exist
    assert_contains "$(cat "$log")" "Unknown source artifact class" \
        "unknown artifact class diagnostic"
}

# The test-tools disk is optional and recipe-driven. These checks pin the
# default recipe, canonical ordering, duplicate handling, and invalid-input
# diagnostics without building the image.
test_test_tools_recipe_selection() {
    local log="$TEST_TMP/test-tools-recipe.log"

    assert_eq "$(env -u PREBUILT_TEST_TOOLS_RECIPE bash -c \
        '. scripts/prebuilt/test-tools-recipe.sh; prebuilt_test_tools_recipe_key')" \
        "x11,directfb2" \
        "default test-tools recipe key"

    assert_eq "$(PREBUILT_TEST_TOOLS_RECIPE='directfb2,x11' \
        prebuilt_test_tools_recipe_key)" \
        "x11,directfb2" \
        "test-tools recipe key should be canonicalized"

    assert_eq "$(PREBUILT_TEST_TOOLS_RECIPE='x11,x11' \
        prebuilt_test_tools_recipe_key)" \
        "x11" \
        "duplicate test-tools recipe entries should collapse"

    expect_failure "empty test-tools recipe" "$log" \
        env PREBUILT_TEST_TOOLS_RECIPE= bash -c \
        '. scripts/prebuilt/test-tools-recipe.sh; prebuilt_test_tools_recipe_key'
    assert_contains "$(cat "$log")" "Empty test-tools recipe" \
        "empty test-tools recipe diagnostic"

    expect_failure "unknown test-tools recipe" "$log" \
        env PREBUILT_TEST_TOOLS_RECIPE=unknown bash -c \
        '. scripts/prebuilt/test-tools-recipe.sh; prebuilt_test_tools_recipe_key'
    assert_contains "$(cat "$log")" "Unknown test-tools recipe entry" \
        "unknown test-tools recipe diagnostic"
}

# CI plan files encode class names as shell variable prefixes. Test the helper
# mapping directly so class names such as test-tools keep using safe variable
# names and do not drift from the planner/materializer contract.
test_prebuilt_ci_helper_contract() {
    local inputs
    local total
    local unique

    assert_eq "$(prebuilt_plan_class_var_name test-tools action)" \
        "test_tools_action" \
        "test-tools plan variable name"
    prebuilt_plan_set_class_var test-tools action build
    assert_eq "$(prebuilt_plan_get_class_var test-tools action)" "build" \
        "test-tools plan variable round trip"

    if ! prebuilt_plan_class_var_matches test_tools_action test-tools action current_recipe_key; then
        fail "test-tools action key should match generated plan vars"
    fi
    if prebuilt_plan_class_var_matches test_tools_action rootfs action current_recipe_key; then
        fail "test-tools action key should not match rootfs plan vars"
    fi

    assert_eq "$(prebuilt_class_recipe_entries test-tools)" \
        "test-tools.img.recipe-key" \
        "test-tools recipe manifest entry"
    assert_eq "$(prebuilt_artifact_recipe_stamp Image)" \
        ".stamps/prebuilt-local/Image.recipe-key" \
        "Image recipe stamp path"

    inputs=$(PREBUILT_TEST_TOOLS_RECIPE=x11 prebuilt_class_inputs test-tools)
    assert_contains "$inputs" "configs/x11.config" \
        "x11 test-tools inputs"
    assert_not_contains "$inputs" "configs/meson-riscv-cross-file" \
        "x11 test-tools inputs"

    inputs=$(PREBUILT_TEST_TOOLS_RECIPE=directfb2 prebuilt_class_inputs test-tools)
    assert_not_contains "$inputs" "configs/x11.config" \
        "directfb2 test-tools inputs"
    assert_contains "$inputs" "configs/meson-riscv-cross-file" \
        "directfb2 test-tools inputs"

    inputs=$(prebuilt_inputs)
    total=$(printf '%s\n' "$inputs" | sed '/^$/d' | wc -l | tr -d ' ')
    unique=$(printf '%s\n' "$inputs" | sed '/^$/d' | sort -u | wc -l | tr -d ' ')
    assert_eq "$total" "$unique" "prebuilt_inputs should be de-duplicated"
}

# The recipe manifest is more useful than checking only the final SHA: when a
# recipe key changes, the manifest should show exactly which selected input
# participated in the class recipe.
test_recipe_manifest_tracks_selected_inputs() {
    local manifest

    manifest=$(PREBUILT_TEST_TOOLS_RECIPE=x11 \
        prebuilt_class_recipe_manifest test-tools)
    assert_contains "$manifest" "configs/x11.config" \
        "x11 recipe manifest"
    assert_not_contains "$manifest" "configs/meson-riscv-cross-file" \
        "x11 recipe manifest"
    assert_contains "$manifest" "test-tools.recipe" \
        "x11 recipe manifest"
    assert_contains "$manifest" "artifact-recipe.env:TEST_TOOLS_SIZE_MB" \
        "x11 recipe manifest"

    manifest=$(PREBUILT_TEST_TOOLS_RECIPE=directfb2 \
        prebuilt_class_recipe_manifest test-tools)
    assert_not_contains "$manifest" "configs/x11.config" \
        "directfb2 recipe manifest"
    assert_contains "$manifest" "configs/meson-riscv-cross-file" \
        "directfb2 recipe manifest"
    assert_contains "$manifest" "test-tools.recipe" \
        "directfb2 recipe manifest"
}

# Local CLI validation should fail before expensive source checkout/build work.
# These cases cover option combinations that are easy to break while extending
# scripts/build-artifacts.sh.
test_build_artifacts_cli_validation() {
    local log="$TEST_TMP/build-artifacts.log"

    expect_success "build-artifacts help" "$log" \
        scripts/build-artifacts.sh --help
    assert_contains "$(cat "$log")" "Usage:" "build-artifacts help"

    expect_failure "recipe option without target" "$log" \
        scripts/build-artifacts.sh image --x11
    assert_contains "$(cat "$log")" \
        "test-tools recipe options require the test-tools or all target" \
        "recipe option without target diagnostic"

    expect_failure "--no-ext4 without rootfs target" "$log" \
        scripts/build-artifacts.sh image --no-ext4
    assert_contains "$(cat "$log")" \
        "--no-ext4 requires the rootfs or all target" \
        "--no-ext4 diagnostic"

    expect_failure "unknown build-artifacts target" "$log" \
        scripts/build-artifacts.sh does-not-exist
    assert_contains "$(cat "$log")" "Unknown option or target" \
        "unknown build-artifacts target diagnostic"
}

# A dry-run Makefile is enough to inspect the generated recipe. This pins the
# previous review concern that PREBUILT_URL must be expanded by Make and passed
# to the shell as an environment value, without performing any network access.
test_make_download_rule_uses_make_expansion() {
    local makefile="$TEST_TMP/external.mk"
    local log="$TEST_TMP/external-make.log"

    printf 'include mk/external.mk\n' > "$makefile"
    expect_success "mk/external Image rule dry run" "$log" \
        make -n -B -f "$makefile" Image \
        PREBUILT_URL=https://example.invalid/prebuilt

    assert_contains "$(cat "$log")" \
        'PREBUILT_URL="https://example.invalid/prebuilt" scripts/prebuilt/download-release-artifact.sh "Image"' \
        "mk/external download command"
    assert_not_contains "$(cat "$log")" '$(PREBUILT_URL)' \
        "mk/external download command"
}

# The Buildroot source lock is a symlink in the current design. Keep the ignore
# rule matching symlinks as well as old-style directory locks so interrupted
# builds do not leave noisy untracked lock paths in developer worktrees.
test_buildroot_lock_path_is_ignored() {
    local tmp="$TEST_TMP/gitignore-lock"
    local status

    mkdir "$tmp"
    set +e
    (
        cd "$tmp"
        git init -q
        cp "$REPO_ROOT/.gitignore" .gitignore
        ln -s "semu-buildroot:test:1:0:token" .semu-buildroot.lock
        git check-ignore -q .semu-buildroot.lock
    )
    status=$?
    set -e

    [ "$status" -eq 0 ] || fail ".semu-buildroot.lock symlink should be ignored"
}

# rootfs_ext4.sh intentionally normalizes the whole staged filesystem to root
# ownership before mkfs, matching the historical ext4.img policy. This static
# check avoids running fakeroot/mkfs while still catching the accidental case
# where chown is only applied when an extra overlay directory is provided.
test_rootfs_ext4_normalizes_ownership_unconditionally() {
    local in_extra=false
    local after_extra=false
    local chown_in_extra=false
    local chown_after_extra=false
    local line

    while IFS= read -r line; do
        case "$line" in
            "if [ -n \"\$SEMU_EXTRA_DIR\" ]; then")
                in_extra=true
                ;;
            "fi")
                if [ "$in_extra" = true ]; then
                    in_extra=false
                    after_extra=true
                fi
                ;;
            '"$SEMU_ROOTFS_CHOWN" -R 0:0 .'|'    "$SEMU_ROOTFS_CHOWN" -R 0:0 .')
                if [ "$in_extra" = true ]; then
                    chown_in_extra=true
                fi
                if [ "$after_extra" = true ]; then
                    chown_after_extra=true
                fi
                ;;
        esac
    done < scripts/rootfs_ext4.sh

    [ "$chown_after_extra" = true ] || \
        fail "rootfs_ext4.sh should chown the staged rootfs outside the extra-dir branch"
    [ "$chown_in_extra" = false ] || \
        fail "rootfs_ext4.sh should not limit rootfs ownership normalization to extra files"
}


# rootfs_ext4.sh runs several external tools under fakeroot. Keep each tool
# selectable by the caller so CI can provide one coherent toolchain on hosts
# where mixing system and package-manager binaries breaks fakeroot injection.
test_rootfs_ext4_tools_are_selectable() {
    local script

    script=$(cat scripts/rootfs_ext4.sh)

    assert_contains "$script" 'ROOTFS_CPIO="${ROOTFS_CPIO:-cpio}"' \
        "rootfs_ext4 cpio hook"
    assert_contains "$script" 'ROOTFS_CP="${ROOTFS_CP:-cp}"' \
        "rootfs_ext4 cp hook"
    assert_contains "$script" 'ROOTFS_CHOWN="${ROOTFS_CHOWN:-chown}"' \
        "rootfs_ext4 chown hook"
    assert_contains "$script" 'ROOTFS_FAKEROOT_SHELL="${ROOTFS_FAKEROOT_SHELL:-/bin/sh}"' \
        "rootfs_ext4 fakeroot shell hook"
    assert_contains "$script" '"$SEMU_ROOTFS_CP" -a "$SEMU_EXTRA_DIR"/. .' \
        "rootfs_ext4 cp command should use caller-selected tool"
    assert_contains "$script" '"$SEMU_ROOTFS_CHOWN" -R 0:0 .' \
        "rootfs_ext4 chown command should use caller-selected tool"
    assert_contains "$script" 'fakeroot "$ROOTFS_FAKEROOT_SHELL" "$FAKEROOT_SCRIPT"' \
        "rootfs_ext4 fakeroot shell command should use caller-selected tool"
}

# The Makefile probe must exercise the same fakeroot tool hooks used by the
# ext4 image path. A trivial shell-only probe can pass even when the later
# fakeroot child fails to run cp/chown on a host such as macOS 26.
test_makefile_uses_rootfs_fakeroot_probe_helper() {
    local makefile

    makefile=$(cat Makefile)

    assert_contains "$makefile" 'scripts/check-rootfs-fakeroot.sh' \
        "Makefile should delegate fakeroot probing to the rootfs helper"
    assert_not_contains "$makefile" 'fakeroot /bin/sh -c :' \
        "Makefile should not use a shell-only fakeroot probe"

    local ext4_rule
    ext4_rule=$(awk '/^ext4.img:/{capture=1} capture{print} capture && /^$/{exit}' Makefile)
    assert_contains "$ext4_rule" 'ROOTFS_FAKEROOT_SHELL="$(ROOTFS_FAKEROOT_SHELL)"' \
        "Makefile ext4 rule should pass the fakeroot shell hook"
    assert_contains "$ext4_rule" 'ROOTFS_CP="$(ROOTFS_CP)"' \
        "Makefile ext4 rule should pass the cp hook"
    assert_contains "$ext4_rule" 'ROOTFS_CHOWN="$(ROOTFS_CHOWN)"' \
        "Makefile ext4 rule should pass the chown hook"
}


# macOS CI should use the rootfs_ext4.sh hooks instead of hardcoding platform
# branches in the helper itself. The selected binaries must come from one
# Homebrew toolchain so fakeroot injects into matching arm64 processes on both
# macOS 15 and macOS 26 runners.
test_macos_setup_exports_rootfs_tool_hooks() {
    local action

    action=$(cat .github/actions/setup-semu/action.yml)

    assert_contains "$action" 'brew install make dtc expect fakeroot bash cpio coreutils e2fsprogs sdl2' \
        "macOS setup should install the rootfs toolchain"
    assert_contains "$action" 'echo "ROOTFS_FAKEROOT_SHELL=$rootfs_fakeroot_shell" >> "$GITHUB_ENV"' \
        "macOS setup should export the fakeroot shell hook"
    assert_contains "$action" 'echo "ROOTFS_CPIO=$rootfs_cpio" >> "$GITHUB_ENV"' \
        "macOS setup should export the cpio hook"
    assert_contains "$action" 'echo "ROOTFS_CP=$rootfs_cp" >> "$GITHUB_ENV"' \
        "macOS setup should export the cp hook"
    assert_contains "$action" 'echo "ROOTFS_CHOWN=$rootfs_chown" >> "$GITHUB_ENV"' \
        "macOS setup should export the chown hook"
}

# The materializer consumes planner output in several CI jobs. Use tiny fake
# plans to check parser behavior, unknown-key rejection, and the guard that
# prevents source builds in jobs that should only materialize cached artifacts.
test_materializer_plan_validation() {
    local plan="$TEST_TMP/prebuilt-plan.env"
    local log="$TEST_TMP/materialize.log"

    {
        printf 'plan_version=1\n'
        printf 'image_action=use-action-cache\n'
        printf 'rootfs_action=use-action-cache\n'
        printf 'test_tools_action=use-action-cache\n'
    } > "$plan"
    expect_success "materializer accepts no-op cache plan" "$log" \
        env PREBUILT_PLAN_FILE="$plan" .ci/prebuilt/materialize-artifacts.sh

    {
        printf 'plan_version=1\n'
        printf 'image_action=use-action-cache\n'
        printf 'rootfs_action=use-action-cache\n'
        printf 'test_tools_action=use-action-cache\n'
        printf 'unknown_key=value\n'
    } > "$plan"
    expect_failure "materializer rejects unknown plan key" "$log" \
        env PREBUILT_PLAN_FILE="$plan" .ci/prebuilt/materialize-artifacts.sh
    assert_contains "$(cat "$log")" "Unknown prebuilt plan key" \
        "unknown materializer plan key diagnostic"

    {
        printf 'plan_version=1\n'
        printf 'image_action=use-action-cache\n'
        printf 'rootfs_action=build\n'
        printf 'current_rootfs_recipe_key=dummy-rootfs-key\n'
        printf 'test_tools_action=use-action-cache\n'
    } > "$plan"
    expect_failure "materializer honors PREBUILT_FORBID_BUILD" "$log" \
        env PREBUILT_PLAN_FILE="$plan" PREBUILT_FORBID_BUILD=1 \
        .ci/prebuilt/materialize-artifacts.sh
    assert_contains "$(cat "$log")" "PREBUILT_FORBID_BUILD forbids source builds" \
        "forbid-build diagnostic"
    assert_contains "$(cat "$log")" "rootfs_action=build" \
        "forbid-build class diagnostic"
}

# The Buildroot source tree can be shared by Image/rootfs/test-tools builds. The
# lock must publish an owner token atomically and remove only that token on a
# normal release.
test_lock_acquire_release_contract() (
    . scripts/prebuilt/artifact-recipes.sh

    local tmp="$TEST_TMP/lock-acquire-release"
    mkdir "$tmp"
    BUILDROOT_LOCK_PATH="$tmp/lock"

    buildroot_acquire_lock
    [ -L "$BUILDROOT_LOCK_PATH" ] || fail "lock path should be a symlink"
    assert_eq "$(readlink "$BUILDROOT_LOCK_PATH")" "$BUILDROOT_LOCK_TOKEN" \
        "lock owner token"
    buildroot_release_lock
    [ ! -e "$BUILDROOT_LOCK_PATH" ] || fail "lock should be removed after release"
)

# Artifact helpers may nest source-build operations in one process. Reentrant
# acquisition should only bump depth and must not remove the symlink until the
# outermost release.
test_lock_reentrant_contract() (
    . scripts/prebuilt/artifact-recipes.sh

    local tmp="$TEST_TMP/lock-reentrant"
    mkdir "$tmp"
    BUILDROOT_LOCK_PATH="$tmp/lock"

    buildroot_acquire_lock
    local token=$BUILDROOT_LOCK_TOKEN
    buildroot_acquire_lock
    assert_eq "$BUILDROOT_LOCK_DEPTH" "2" "reentrant lock depth"
    buildroot_release_lock
    [ -L "$BUILDROOT_LOCK_PATH" ] || fail "reentrant first release should keep lock"
    assert_eq "$(readlink "$BUILDROOT_LOCK_PATH")" "$token" \
        "reentrant lock token"
    buildroot_release_lock
    [ ! -e "$BUILDROOT_LOCK_PATH" ] || fail "reentrant final release should remove lock"
)

# Release uses compare-and-delete semantics. If another owner replaces the lock
# path before release, the old owner must leave the new token alone.
test_lock_release_only_removes_own_token() (
    . scripts/prebuilt/artifact-recipes.sh

    local tmp="$TEST_TMP/lock-release-token"
    mkdir "$tmp"
    BUILDROOT_LOCK_PATH="$tmp/lock"

    buildroot_acquire_lock
    rm -f "$BUILDROOT_LOCK_PATH"
    ln -s "semu-buildroot:foreign:12345:0:fresh" "$BUILDROOT_LOCK_PATH"
    buildroot_release_lock
    assert_eq "$(readlink "$BUILDROOT_LOCK_PATH")" \
        "semu-buildroot:foreign:12345:0:fresh" \
        "release must not delete a replaced lock"
)

# Stale locks are reported but not automatically removed. That is intentional:
# deleting after a read/compare check can race with a fresh owner that acquired
# the path after the stale check.
test_lock_reports_stale_without_deleting() {
    local tmp="$TEST_TMP/lock-stale"
    local log="$tmp/stale.log"
    local host
    local token
    local status

    mkdir "$tmp"
    host=$(buildroot_lock_host)
    token="semu-buildroot:$host:99999999:0:dead"
    ln -s "$token" "$tmp/lock"

    set +e
    (
        . scripts/prebuilt/artifact-recipes.sh
        BUILDROOT_LOCK_PATH="$tmp/lock"
        buildroot_acquire_lock
    ) >"$log" 2>&1
    status=$?
    set -e

    [ "$status" -ne 0 ] || fail "stale lock acquire should fail"
    [ -L "$tmp/lock" ] || fail "stale lock should not be deleted"
    assert_eq "$(readlink "$tmp/lock")" "$token" \
        "stale lock token should be preserved"
    assert_contains "$(cat "$log")" "source build lock appears stale" \
        "stale lock diagnostic"
}

# Old directory locks, hand-created files, and other foreign paths should stop
# the build before the protected payload runs. The diagnostic tells the user
# which path needs manual cleanup.
test_lock_reports_unowned_without_running_payload() {
    local tmp="$TEST_TMP/lock-unowned"
    local log="$tmp/unowned.log"
    local status

    mkdir -p "$tmp/lock"

    set +e
    (
        . scripts/prebuilt/artifact-recipes.sh
        BUILDROOT_LOCK_PATH="$tmp/lock"
        with_buildroot_lock touch "$tmp/payload"
    ) >"$log" 2>&1
    status=$?
    set -e

    [ "$status" -ne 0 ] || fail "unowned lock acquire should fail"
    [ -d "$tmp/lock" ] || fail "unowned lock directory should not be deleted"
    [ ! -e "$tmp/payload" ] || fail "payload ran despite lock acquisition failure"
    assert_contains "$(cat "$log")" "not an owned semu lock" \
        "unowned lock diagnostic"

    rm -rf "$tmp/lock"
    (
        . scripts/prebuilt/artifact-recipes.sh
        BUILDROOT_LOCK_PATH="$tmp/lock"
        with_buildroot_lock touch "$tmp/payload"
    ) >"$log" 2>&1
    [ -e "$tmp/payload" ] || fail "payload command should run after acquiring a valid lock"
}

# Contention should be safe and observable: one process waits on the existing
# owner token, then acquires only after the holder releases it.
test_lock_waiter_acquires_after_release() {
    local tmp="$TEST_TMP/lock-contention"
    local holder_log="$tmp/holder.log"
    local waiter_log="$tmp/waiter.log"
    local holder
    local waiter
    local saw_wait=false
    local i

    mkdir "$tmp"
    (
        . scripts/prebuilt/artifact-recipes.sh
        BUILDROOT_LOCK_PATH="$tmp/lock"
        buildroot_acquire_lock
        touch "$tmp/holder-ready"
        # Keep the holder locked until the parent has observed the waiter
        # report contention. A fixed sleep makes the test depend on scheduler
        # timing: under a slow runner the waiter could start only after the
        # holder released, acquire immediately, and never print the wait
        # message this test is specifically checking.
        while [ ! -e "$tmp/release-holder" ]; do
            sleep 0.05
        done
        buildroot_release_lock
    ) >"$holder_log" 2>&1 &
    holder=$!

    while [ ! -e "$tmp/holder-ready" ]; do
        sleep 0.05
    done

    (
        . scripts/prebuilt/artifact-recipes.sh
        BUILDROOT_LOCK_PATH="$tmp/lock"
        buildroot_acquire_lock
        printf "waiter-acquired\n"
        buildroot_release_lock
    ) >"$waiter_log" 2>&1 &
    waiter=$!

    # Wait until the waiter has actually observed the held lock and emitted the
    # diagnostic. Only then let the holder release. This makes the later
    # assertion deterministic instead of relying on the holder sleep duration.
    for i in {1..100}; do
        if grep -q "Waiting for Buildroot source build lock:" "$waiter_log" 2>/dev/null; then
            saw_wait=true
            break
        fi
        if ! kill -0 "$waiter" 2>/dev/null; then
            break
        fi
        sleep 0.05
    done

    if [ "$saw_wait" != true ]; then
        touch "$tmp/release-holder"
        wait "$holder" || true
        wait "$waiter" || true
        cat "$holder_log" >&2
        cat "$waiter_log" >&2
        fail "waiter did not observe the held lock before acquiring"
    fi

    touch "$tmp/release-holder"
    wait "$waiter"
    wait "$holder"

    assert_contains "$(cat "$waiter_log")" \
        "Waiting for Buildroot source build lock:" \
        "lock waiter diagnostic"
    assert_contains "$(cat "$waiter_log")" "waiter-acquired" \
        "lock waiter completion"
    [ ! -e "$tmp/lock" ] || fail "lock should be removed after contention test"
}

# Source the lock helper into the parent for helper-only calls, such as
# buildroot_lock_host in the stale-lock test. Tests that acquire a lock source
# the helper again inside subshells, so buildroot_acquire_lock can install and
# own an EXIT trap without replacing the parent test runner cleanup trap.
. scripts/prebuilt/artifact-recipes.sh

run_test "artifact class contract" test_artifact_class_contract
run_test "test-tools recipe selection" test_test_tools_recipe_selection
run_test "prebuilt CI helper contract" test_prebuilt_ci_helper_contract
run_test "recipe manifest selected inputs" test_recipe_manifest_tracks_selected_inputs
run_test "build-artifacts CLI validation" test_build_artifacts_cli_validation
run_test "mk/external download rule" test_make_download_rule_uses_make_expansion
run_test "buildroot lock gitignore rule" test_buildroot_lock_path_is_ignored
run_test "rootfs ext4 ownership policy" test_rootfs_ext4_normalizes_ownership_unconditionally
run_test "rootfs ext4 tool selection" test_rootfs_ext4_tools_are_selectable
run_test "rootfs fakeroot probe helper" test_makefile_uses_rootfs_fakeroot_probe_helper
run_test "macOS setup rootfs hooks" test_macos_setup_exports_rootfs_tool_hooks
run_test "materializer plan validation" test_materializer_plan_validation
run_test "buildroot lock acquire/release" test_lock_acquire_release_contract
run_test "buildroot lock reentrant release" test_lock_reentrant_contract
run_test "buildroot lock release token check" test_lock_release_only_removes_own_token
run_test "buildroot lock stale detection" test_lock_reports_stale_without_deleting
run_test "buildroot lock unowned rejection" test_lock_reports_unowned_without_running_payload
run_test "buildroot lock contention" test_lock_waiter_acquires_after_release

printf '  PASS\tbuild system tests\n'
