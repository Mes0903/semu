#!/usr/bin/env bash

# build-artifacts.sh is the user-facing source-build entrypoint. Normal Make
# flows such as `make check` do not call this script for missing guest artifacts;
# they use mk/external.mk to download the rolling prebuilt release instead.
# CI cache/stamp decisions live under .ci/prebuilt and are intentionally outside
# this local CLI.
set -euo pipefail

SCRIPT_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH='' cd -- "$SCRIPT_DIR/.." && pwd)

cd "$REPO_ROOT"
. "$REPO_ROOT/scripts/prebuilt/artifact-recipes.sh"

TEST_TOOLS_RECIPE_EXPLICIT=0
TEST_TOOLS_RECIPE=

add_test_tools_recipe_entry() {
    local entry=$1

    if [[ $TEST_TOOLS_RECIPE_EXPLICIT -eq 0 ]]; then
        TEST_TOOLS_RECIPE=
        TEST_TOOLS_RECIPE_EXPLICIT=1
    fi

    case ",$TEST_TOOLS_RECIPE," in
        *,"$entry",*)
            ;;
        ,,)
            TEST_TOOLS_RECIPE=$entry
            ;;
        *)
            TEST_TOOLS_RECIPE=$TEST_TOOLS_RECIPE,$entry
            ;;
    esac
}

show_help() {
    local exit_code=${1:-0}
    local prog

    prog=$(basename "$0")
    cat << HELP
Usage: $prog <image|rootfs|test-tools|all>... [--x11] [--directfb2-test] [--no-ext4] [--clean-build] [--full-rebuild] [--help]

Targets:
  image             Build the Linux Image. This prepares the Buildroot
                    toolchain if needed but does not publish rootfs.cpio or
                    test-tools.img as final outputs.
  rootfs            Build Buildroot rootfs.cpio and, unless --no-ext4 is
                    given, derive ext4.img from it.
  test-tools        Build test-tools.img. By default this uses the canonical
                    X11 + DirectFB2 test payload recipe.
  all               Build image, rootfs, and canonical test-tools.

Options:
  --x11             Select the X11/C++ runtime recipe entry for test-tools. When
                    any test-tools recipe flag is given, only selected
                    entries are included.
  --directfb2-test  Select the DirectFB2 test payload recipe entry for test-tools.
  --no-ext4         With rootfs, skip ext4.img generation and produce only
                    rootfs.cpio (matches the legacy ENABLE_EXTERNAL_ROOT=0 path).
  --clean-build     Remove selected raw artifact outputs and Buildroot
                    output directories while preserving source checkouts and
                    download cache.
  --full-rebuild    Remove selected raw artifact outputs and external
                    source/build trees before building so they are cloned and
                    built again from pinned revisions.
  --help            Show this message.
HELP
    exit "$exit_code"
}

BUILD_LINUX=0
BUILD_ROOTFS=0
BUILD_TEST_TOOLS=0
NO_EXT4=0
CLEAN_BUILD=0
FULL_REBUILD=0
TARGET_SPECIFIED=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        image)
            BUILD_LINUX=1
            TARGET_SPECIFIED=1
            ;;
        rootfs)
            BUILD_ROOTFS=1
            TARGET_SPECIFIED=1
            ;;
        test-tools)
            BUILD_TEST_TOOLS=1
            TARGET_SPECIFIED=1
            ;;
        all)
            BUILD_LINUX=1
            BUILD_ROOTFS=1
            BUILD_TEST_TOOLS=1
            TARGET_SPECIFIED=1
            ;;
        --x11)
            add_test_tools_recipe_entry x11
            ;;
        --directfb2-test)
            add_test_tools_recipe_entry directfb2
            ;;
        --no-ext4)
            NO_EXT4=1
            ;;
        --clean-build)
            CLEAN_BUILD=1
            ;;
        --full-rebuild)
            FULL_REBUILD=1
            ;;
        --help|-h)
            show_help 0
            ;;
        *)
            echo "Unknown option or target: $1"
            show_help 1
            ;;
    esac
    shift
done

if [[ $TARGET_SPECIFIED -eq 0 ]]; then
    echo "Error: No build target specified. Use image, rootfs, test-tools, or all."
    show_help 1
fi

if [[ $TEST_TOOLS_RECIPE_EXPLICIT -eq 1 && $BUILD_TEST_TOOLS -eq 0 ]]; then
    echo "Error: test-tools recipe options require the test-tools or all target."
    show_help 1
fi

if [[ $NO_EXT4 -eq 1 && $BUILD_ROOTFS -eq 0 ]]; then
    echo "Error: --no-ext4 requires the rootfs or all target."
    show_help 1
fi

if [[ $BUILD_TEST_TOOLS -eq 1 ]]; then
    if [[ $TEST_TOOLS_RECIPE_EXPLICIT -eq 1 ]]; then
        export PREBUILT_TEST_TOOLS_RECIPE=$TEST_TOOLS_RECIPE
    fi
    prebuilt_test_tools_recipe_key >/dev/null
fi

remove_existing_paths() {
    local label=$1
    shift
    local paths=()
    local path

    for path in "$@"; do
        if [[ -e $path ]]; then
            paths+=("$path")
        fi
    done

    if [[ ${#paths[@]} -eq 0 ]]; then
        return
    fi

    echo "Removing $label:"
    printf '  %s\n' "${paths[@]}"
    rm -rf -- "${paths[@]}"
}

append_class_outputs() {
    local class=$1
    local output

    while IFS= read -r output; do
        outputs+=("$output")
    done < <(source_artifact_class_outputs "$class")
}

remove_selected_artifact_outputs() {
    local outputs=()

    if [[ $BUILD_LINUX -eq 1 ]]; then
        append_class_outputs image
    fi

    if [[ $BUILD_ROOTFS -eq 1 ]]; then
        append_class_outputs rootfs
        if [[ $NO_EXT4 -eq 0 ]]; then
            outputs+=(ext4.img)
        fi
    fi

    if [[ $BUILD_TEST_TOOLS -eq 1 ]]; then
        append_class_outputs test-tools
    fi

    remove_existing_paths "selected raw artifact outputs" "${outputs[@]}"
}

derive_local_ext4_from_rootfs() {
    if [[ $NO_EXT4 -eq 1 ]]; then
        echo "Skipping ext4.img build (--no-ext4)"
        return
    fi

    ASSERT ./scripts/rootfs_ext4.sh ./rootfs.cpio ./ext4.img
}

if [[ $CLEAN_BUILD -eq 1 || $FULL_REBUILD -eq 1 ]]; then
    remove_selected_artifact_outputs
fi

if [[ $FULL_REBUILD -eq 1 ]]; then
    if [[ $BUILD_LINUX -eq 1 || $BUILD_ROOTFS -eq 1 || $BUILD_TEST_TOOLS -eq 1 ]]; then
        remove_existing_paths "Buildroot source tree for full rebuild" buildroot
    fi

    if [[ $BUILD_LINUX -eq 1 ]]; then
        remove_existing_paths "Linux source tree for full rebuild" linux
    fi

    if [[ $BUILD_TEST_TOOLS -eq 1 ]]; then
        remove_existing_paths "test-tools source/build trees for full rebuild" \
            DirectFB2 DirectFB-examples directfb extra_packages
    fi
elif [[ $CLEAN_BUILD -eq 1 ]]; then
    if [[ $BUILD_LINUX -eq 1 || $BUILD_ROOTFS -eq 1 ]]; then
        remove_existing_paths "default Buildroot output for clean build" \
            "$BUILDROOT_DEFAULT_OUTPUT" buildroot/output
    fi

    if [[ $BUILD_TEST_TOOLS -eq 1 ]]; then
        remove_existing_paths "test-tools Buildroot output for clean build" \
            "$BUILDROOT_TEST_TOOLS_OUTPUT"
    fi
fi

if [[ $BUILD_LINUX -eq 1 ]]; then
    build_source_artifact_class image
    OK
fi

if [[ $BUILD_ROOTFS -eq 1 ]]; then
    build_source_artifact_class rootfs
    derive_local_ext4_from_rootfs
    OK
fi

if [[ $BUILD_TEST_TOOLS -eq 1 ]]; then
    build_source_artifact_class test-tools
    OK
fi
