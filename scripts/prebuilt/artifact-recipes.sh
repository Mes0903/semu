#!/usr/bin/env bash

# This recipe library is sourced by scripts/build-artifacts.sh and the internal
# prebuilt plan builder. It intentionally enables fail-fast shell options for
# the caller before any artifact work starts.
set -euo pipefail

ARTIFACT_RECIPES_DIR=$(CDPATH='' cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)

# Load build recipe pins and recipe selection helpers before defining build
# helpers. CI owns recipe-key/stamp metadata separately under .ci/prebuilt; this
# file keeps the build-facing source configuration in one place.
{
    . "$ARTIFACT_RECIPES_DIR/artifact-classes.sh"
    . "$ARTIFACT_RECIPES_DIR/artifact-recipe.env"
    . "$ARTIFACT_RECIPES_DIR/test-tools-recipe.sh"
}

ASSERT() {
    local res

    if "$@"; then
        return 0
    else
        res=$?
    fi

    echo 'Assert failed: "' "$@" '"'
    exit "$res"
}

PASS_COLOR='\e[32;01m'
NO_COLOR='\e[0m'
OK() {
    printf ' [ %b OK %b ]\n' "$PASS_COLOR" "$NO_COLOR"
}

# Match Buildroot/Linux parallelism to the host by default. Keep the value in
# one variable so every underlying make invocation uses the same job policy.
if command -v nproc >/dev/null 2>&1; then
    build_jobs=$(nproc)
else
    build_jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || printf '1')
fi
PARALLEL="-j$build_jobs"

BUILDROOT_DEFAULT_OUTPUT=${BUILDROOT_DEFAULT_OUTPUT:-buildroot/output-default}
BUILDROOT_TEST_TOOLS_OUTPUT=${BUILDROOT_TEST_TOOLS_OUTPUT:-buildroot/output-test-tools}

# buildroot/ is shared mutable state: checkout, config, host toolchain, and
# rootfs output all live there. Serialize separate shell processes that might
# race on that tree, while still letting Buildroot use its own internal -j
# parallelism inside one locked operation.
BUILDROOT_LOCK_PATH=${BUILDROOT_LOCK_PATH:-.semu-buildroot.lock}
BUILDROOT_LOCK_DEPTH=0
BUILDROOT_LOCK_TOKEN=

buildroot_lock_host() {
    hostname 2>/dev/null || uname -n 2>/dev/null || printf 'unknown'
}

buildroot_new_lock_token() {
    printf 'semu-buildroot:%s:%s:%s:%s\n' \
        "$(buildroot_lock_host)" "$$" "$(date +%s)" "$RANDOM$RANDOM"
}

buildroot_lock_owner() {
    readlink "$BUILDROOT_LOCK_PATH" 2>/dev/null || true
}

buildroot_lock_owner_field() {
    local owner=$1
    local field=$2
    local prefix host pid started nonce

    IFS=: read -r prefix host pid started nonce <<< "$owner"
    case "$prefix" in
        semu-buildroot) ;;
        *) return 1 ;;
    esac

    case "$field" in
        host) printf '%s\n' "$host" ;;
        pid) printf '%s\n' "$pid" ;;
        *) return 1 ;;
    esac
}

buildroot_report_unowned_lock() {
    cat >&2 << EOF
Error: Buildroot source build lock path is not an owned semu lock:
  $BUILDROOT_LOCK_PATH

Check that no semu artifact source build is running, then remove the lock:
  rm -rf "$BUILDROOT_LOCK_PATH"
EOF
}

buildroot_report_stale_lock() {
    local owner=$1

    cat >&2 << EOF
Error: Buildroot source build lock appears stale:
  $BUILDROOT_LOCK_PATH -> $owner

Portable shell/filesystem operations cannot remove this lock with an atomic
owner check. Check that no semu artifact source build is running, then remove
the lock:
  rm -f "$BUILDROOT_LOCK_PATH"
EOF
}

buildroot_lock_owner_is_valid() {
    local owner=$1
    local host
    local pid

    host=$(buildroot_lock_owner_field "$owner" host) || return 1
    pid=$(buildroot_lock_owner_field "$owner" pid) || return 1

    [ -n "$host" ] || return 1
    case "$pid" in
        ''|*[!0-9]*) return 1 ;;
    esac
}

buildroot_lock_path_is_unowned() {
    [ -e "$BUILDROOT_LOCK_PATH" ] && [ ! -L "$BUILDROOT_LOCK_PATH" ]
}

buildroot_lock_is_known_stale() {
    local owner=$1
    local host
    local pid

    host=$(buildroot_lock_owner_field "$owner" host) || return 1
    pid=$(buildroot_lock_owner_field "$owner" pid) || return 1

    [ "$host" = "$(buildroot_lock_host)" ] || return 1
    case "$pid" in
        ''|*[!0-9]*) return 0 ;;
    esac

    ! kill -0 "$pid" 2>/dev/null
}

buildroot_force_release_lock() {
    if [ "${BUILDROOT_LOCK_DEPTH:-0}" -gt 0 ]; then
        if [ "$(buildroot_lock_owner)" = "$BUILDROOT_LOCK_TOKEN" ]; then
            rm -f "$BUILDROOT_LOCK_PATH"
        fi
        BUILDROOT_LOCK_DEPTH=0
        BUILDROOT_LOCK_TOKEN=
    fi
}

buildroot_release_lock() {
    if [ "${BUILDROOT_LOCK_DEPTH:-0}" -gt 1 ]; then
        BUILDROOT_LOCK_DEPTH=$((BUILDROOT_LOCK_DEPTH - 1))
        return
    fi

    buildroot_force_release_lock
}

buildroot_acquire_lock() {
    local waited=false
    local owner

    if [ "${BUILDROOT_LOCK_DEPTH:-0}" -gt 0 ]; then
        BUILDROOT_LOCK_DEPTH=$((BUILDROOT_LOCK_DEPTH + 1))
        return 0
    fi

    if buildroot_lock_path_is_unowned; then
        buildroot_report_unowned_lock
        return 1
    fi

    BUILDROOT_LOCK_TOKEN=$(buildroot_new_lock_token)
    while ! ln -s "$BUILDROOT_LOCK_TOKEN" "$BUILDROOT_LOCK_PATH" 2>/dev/null; do
        owner=$(buildroot_lock_owner)
        if [ -z "$owner" ] || ! buildroot_lock_owner_is_valid "$owner"; then
            buildroot_report_unowned_lock
            return 1
        fi
        if buildroot_lock_is_known_stale "$owner"; then
            buildroot_report_stale_lock "$owner"
            return 1
        fi
        if [ "$waited" = false ]; then
            echo "Waiting for Buildroot source build lock: $owner"
            waited=true
        fi
        sleep 1
    done

    if [ "$(buildroot_lock_owner)" != "$BUILDROOT_LOCK_TOKEN" ]; then
        buildroot_report_unowned_lock
        return 1
    fi

    BUILDROOT_LOCK_DEPTH=1
    # The build recipe is sourced by simple drivers that do not install their
    # own EXIT trap. While a lock is held, this library owns the caller EXIT
    # trap so interrupted builds do not leave Buildroot locked forever.
    trap buildroot_force_release_lock EXIT
}

with_buildroot_lock() {
    local status

    if ! buildroot_acquire_lock; then
        exit 1
    fi
    "$@"
    status=$?
    buildroot_release_lock
    if [ "$status" -ne 0 ]; then
        exit "$status"
    fi
}

# Keep external source trees pinned to artifact-recipe.env revisions. A shallow
# fetch is enough for release pins most of the time; fall back to a full fetch
# when the requested revision is not reachable from a shallow fetch.
checkout_repo_rev() {
    local dir=$1
    local repo=$2
    local rev=$3

    if [ ! -d "$dir/.git" ]; then
        echo "Cloning $dir..."
        rm -rf "$dir"
        ASSERT git init "$dir"
        pushd "$dir"
        ASSERT git remote add origin "$repo"
        if ! git fetch --depth=1 origin "$rev"; then
            ASSERT git fetch origin "$rev"
        fi
        ASSERT git checkout --detach FETCH_HEAD
        popd
        return
    fi

    echo "$dir already exists, verifying checkout..."
    pushd "$dir"
    if ! git cat-file -e "$rev^{commit}" 2>/dev/null; then
        if ! git fetch --depth=1 origin "$rev"; then
            ASSERT git fetch origin "$rev"
        fi
    fi
    ASSERT git checkout --detach "$rev"
    popd
}

ensure_buildroot_checkout() {
    checkout_repo_rev buildroot "$BUILDROOT_REPO" "$BUILDROOT_REV"
}

# Reuse Meson build directories when possible. If the old directory was created
# with incompatible options or stale metadata, rebuild it from scratch.
meson_setup_or_reconfigure() {
    local build_dir=$1
    shift

    if [ -f "$build_dir/build.ninja" ]; then
        if ! meson setup --reconfigure "$@" "$build_dir"; then
            echo "Recreating stale Meson build directory: $build_dir"
            rm -rf "$build_dir"
            ASSERT meson setup "$@" "$build_dir"
        fi
    else
        ASSERT meson setup "$@" "$build_dir"
    fi
}

buildroot_make() {
    local output_dir=$1
    shift

    ASSERT mkdir -p "$output_dir"
    ASSERT make -C buildroot O="$PWD/$output_dir" "$@"
}

# The default mode copies the base config; the x11 mode overlays the X11
# fragment used by test-tools variants. Keep default rootfs and test-tools in
# separate Buildroot output directories so `all` can build default ext4.img
# before the larger test-tools profile without contaminating the next run.
configure_buildroot() {
    local mode=${1:-default}
    local output_dir=$2
    local buildroot_config=configs/buildroot.config
    local x11_config=configs/x11.config
    local merge_tool=buildroot/support/kconfig/merge_config.sh

    ASSERT mkdir -p "$output_dir"
    if [[ "$mode" == "x11" ]]; then
        echo "Preparing Buildroot config with X11 fragment..."
        ASSERT "$merge_tool" -m -r -O "$output_dir" "$buildroot_config" "$x11_config"
    else
        echo "Preparing default Buildroot config..."
        ASSERT cp -f "$buildroot_config" "$output_dir/.config"
    fi
}

# Linux and DirectFB builds use the Buildroot-hosted RISC-V cross toolchain.
# Building only the toolchain avoids publishing a rootfs as a side effect.
build_buildroot_toolchain() {
    local buildroot_output=$BUILDROOT_DEFAULT_OUTPUT

    ensure_buildroot_checkout
    configure_buildroot default "$buildroot_output"

    unset LD_LIBRARY_PATH
    buildroot_make "$buildroot_output" olddefconfig
    buildroot_make "$buildroot_output" toolchain "$PARALLEL"
}

# Build the pinned Linux tree with the Buildroot cross toolchain, then publish
# only arch/riscv/boot/Image as the kernel artifact.
do_linux_unlocked() {
    local buildroot_output=$BUILDROOT_DEFAULT_OUTPUT

    build_buildroot_toolchain

    checkout_repo_rev linux "$LINUX_REPO" "$LINUX_REV"

    ASSERT cp -f configs/linux.config linux/.config

    export PATH="$PWD/$buildroot_output/host/bin:$PATH"
    export CROSS_COMPILE=riscv32-buildroot-linux-gnu-
    export ARCH=riscv
    pushd linux
    ASSERT make olddefconfig
    ASSERT make "$PARALLEL"
    ASSERT cp -f arch/riscv/boot/Image ../Image
    popd
}

do_linux() {
    with_buildroot_lock do_linux_unlocked
}

build_buildroot_rootfs() {
    local mode=${1:-default}
    local buildroot_output=$2

    ensure_buildroot_checkout
    configure_buildroot "$mode" "$buildroot_output"
    ASSERT cp -f configs/busybox.config buildroot/busybox.config
    ASSERT cp -f target/init buildroot/fs/cpio/init

    # Buildroot rejects an LD_LIBRARY_PATH containing the current working
    # directory, so keep the environment clean before invoking make.
    unset LD_LIBRARY_PATH
    buildroot_make "$buildroot_output" olddefconfig
    if [[ "$mode" == "x11" && \
          ! -x "$buildroot_output/host/bin/riscv32-buildroot-linux-gnu-g++" ]]; then
        echo "Rebuilding Buildroot final GCC with C++ support..."
        buildroot_make "$buildroot_output" host-gcc-final-dirclean
    fi
    buildroot_make "$buildroot_output" "$PARALLEL"
}

do_rootfs_unlocked() {
    build_buildroot_rootfs default "$BUILDROOT_DEFAULT_OUTPUT"

    # rootfs.cpio is the canonical raw rootfs artifact and also serves as the
    # legacy initramfs payload. Local ext4.img generation is a caller-level
    # convenience layered on top of this raw artifact.
    echo "Publishing rootfs.cpio"
    ASSERT cp -f "$BUILDROOT_DEFAULT_OUTPUT/images/rootfs.cpio" ./rootfs.cpio
}

do_rootfs() {
    with_buildroot_lock do_rootfs_unlocked
}

# DirectFB2 is installed into two places: the Buildroot sysroot so examples can
# link against it, and directfb/ so rootfs_ext4.sh can overlay runtime payloads.
do_directfb() {
    local buildroot_output=$1

    export BUILDROOT_OUT=$PWD/$buildroot_output
    export DIRECTFB_STAGE=$PWD/directfb
    ASSERT mkdir -p directfb

    checkout_repo_rev DirectFB2 "$DIRECTFB2_REPO" "$DIRECTFB2_REV"
    pushd DirectFB2
    sed "s#@BUILDROOT_OUTPUT@#$buildroot_output#g" \
        ../configs/meson-riscv-cross-file > meson-riscv-cross-file
    meson_setup_or_reconfigure build/riscv -Ddrmkms=true --cross-file \
        meson-riscv-cross-file
    ASSERT meson compile -C build/riscv
    ASSERT env DESTDIR="$BUILDROOT_OUT/host/riscv32-buildroot-linux-gnu/sysroot" meson install -C build/riscv
    ASSERT env DESTDIR="$DIRECTFB_STAGE" meson install -C build/riscv
    popd

    checkout_repo_rev DirectFB-examples "$DIRECTFB_EXAMPLES_REPO" \
        "$DIRECTFB_EXAMPLES_REV"
    pushd DirectFB-examples/
    sed "s#@BUILDROOT_OUTPUT@#$buildroot_output#g" \
        ../configs/meson-riscv-cross-file > meson-riscv-cross-file
    meson_setup_or_reconfigure build/riscv --cross-file meson-riscv-cross-file
    ASSERT meson compile -C build/riscv
    ASSERT env DESTDIR="$DIRECTFB_STAGE" meson install -C build/riscv
    popd
}

# Build optional runtime payloads and collect them under extra_packages/. That
# directory is later overlaid into test-tools.img, not the default rootfs.
stage_test_tools_local_env() {
    ASSERT mkdir -p extra_packages/root
    ASSERT cp target/local-env.sh extra_packages/root/
}

do_extra_packages() {
    local buildroot_output=$1

    export PATH="$PWD/$buildroot_output/host/bin:$PATH"
    export CROSS_COMPILE=riscv32-buildroot-linux-gnu-

    ASSERT rm -rf directfb extra_packages
    ASSERT mkdir -p extra_packages/root

    do_directfb "$buildroot_output"
    OK

    if ! find directfb -mindepth 1 -print -quit | grep -q .; then
        echo "Error: DirectFB staging tree is empty."
        exit 1
    fi

    ASSERT cp -r directfb/. extra_packages/
    stage_test_tools_local_env
}

# The x11 test-tools variant needs libstdc++ at runtime, but the default
# guest rootfs should stay small. Stage only the needed C++ libraries.
stage_cxx_runtime() {
    local buildroot_output=$1
    local toolchain_lib=$buildroot_output/host/riscv32-buildroot-linux-gnu/lib
    local libstdcpp=$toolchain_lib/libstdc++.so.6
    local libstdcpp_real

    if [ ! -e "$libstdcpp" ]; then
        echo "Error: libstdc++.so.6 not found in $toolchain_lib"
        exit 1
    fi

    libstdcpp_real=$(readlink "$libstdcpp" || basename "$libstdcpp")
    if [[ "$libstdcpp_real" != /* ]]; then
        libstdcpp_real=$toolchain_lib/$libstdcpp_real
    fi
    ASSERT mkdir -p extra_packages/lib
    ASSERT cp -a "$toolchain_lib/libstdc++.so" "$libstdcpp" \
        "$libstdcpp_real" extra_packages/lib/
}

stage_x11_test_tools_overlay() {
    stage_cxx_runtime "$1"
    stage_test_tools_local_env
    ASSERT mkdir -p extra_packages/etc
    ASSERT touch extra_packages/etc/semu-test-tools-virgl
}

# Translate the normalized test-tools recipe into build decisions. x11 selects
# the Buildroot profile; directfb2 adds the DirectFB2 test payload.
configure_test_tools_recipe_flags() {
    BUILD_X11=0
    BUILD_DIRECTFB_TEST=0

    if prebuilt_test_tools_recipe_includes x11; then
        BUILD_X11=1
    fi
    if prebuilt_test_tools_recipe_includes directfb2; then
        BUILD_DIRECTFB_TEST=1
    fi
}

# Build test-tools.img from a Buildroot rootfs plus optional overlays. The
# recipe can be x11-only, directfb2-only, or the canonical x11,directfb2 set.
do_test_tools_unlocked() {
    local buildroot_mode
    local buildroot_output=$BUILDROOT_TEST_TOOLS_OUTPUT
    local test_tools_rootfs

    configure_test_tools_recipe_flags

    if [[ $BUILD_X11 -eq 1 ]]; then
        buildroot_mode=x11
    else
        buildroot_mode=default
    fi
    build_buildroot_rootfs "$buildroot_mode" "$buildroot_output"
    test_tools_rootfs=./$buildroot_output/images/rootfs.cpio

    if [[ $BUILD_DIRECTFB_TEST -eq 1 ]]; then
        do_extra_packages "$buildroot_output"
        if [[ $BUILD_X11 -eq 1 ]]; then
            stage_x11_test_tools_overlay "$buildroot_output"
        fi
        ASSERT ./scripts/rootfs_ext4.sh "$test_tools_rootfs" ./test-tools.img \
            "$TEST_TOOLS_SIZE_MB" ./extra_packages
    elif [[ $BUILD_X11 -eq 1 ]]; then
        ASSERT rm -rf extra_packages
        stage_x11_test_tools_overlay "$buildroot_output"
        ASSERT ./scripts/rootfs_ext4.sh "$test_tools_rootfs" ./test-tools.img \
            "$TEST_TOOLS_SIZE_MB" ./extra_packages
    else
        ASSERT ./scripts/rootfs_ext4.sh "$test_tools_rootfs" ./test-tools.img \
            "$TEST_TOOLS_SIZE_MB"
    fi
}

do_test_tools() {
    with_buildroot_lock do_test_tools_unlocked
}

source_artifact_class_selected() {
    local want=$1
    local class
    shift

    for class in "$@"; do
        if [[ $class == "$want" ]]; then
            return 0
        fi
    done

    return 1
}

build_source_artifact_class() {
    case "$1" in
        image)
            do_linux
            ;;
        rootfs)
            do_rootfs
            ;;
        test-tools)
            do_test_tools
            ;;
        *)
            echo "[!] Unknown source artifact class: $1" >&2
            return 1
            ;;
    esac
}

# CI asks for classes by name, but this dispatcher keeps execution order fixed.
# That keeps shared Buildroot state deterministic even if caller argument order
# changes.
build_prebuilt_artifact_classes() {
    local class

    if [ "$#" -eq 0 ]; then
        echo "[!] build_prebuilt_artifact_classes requires at least one class" >&2
        return 1
    fi

    for class in "$@"; do
        source_artifact_class_outputs "$class" >/dev/null || return 1
    done

    while IFS= read -r class; do
        if source_artifact_class_selected "$class" "$@"; then
            build_source_artifact_class "$class"
            OK
        fi
    done < <(source_artifact_classes)
}
