#!/usr/bin/env bash
#
# Probe whether fakeroot can run the same external tools used by
# scripts/rootfs_ext4.sh. Some hosts can start fakeroot but fail only when it
# injects into a later child command, so a shell-only probe is not enough.

set -euo pipefail

ROOTFS_CP="${ROOTFS_CP:-cp}"
ROOTFS_CHOWN="${ROOTFS_CHOWN:-chown}"
ROOTFS_FAKEROOT_SHELL="${ROOTFS_FAKEROOT_SHELL:-/bin/sh}"

check_tool() {
    local tool=$1
    local name=$2

    if ! [ -x "$tool" ] && ! command -v "$tool" >/dev/null 2>&1; then
        echo "[!] $name is required for fakeroot rootfs image generation" >&2
        exit 1
    fi
}

check_tool fakeroot fakeroot
check_tool "$ROOTFS_FAKEROOT_SHELL" "fakeroot shell"
check_tool "$ROOTFS_CP" cp
check_tool "$ROOTFS_CHOWN" chown

TEST_DIR="$(mktemp -d "${TMPDIR:-/tmp}/semu-fakeroot-probe.XXXXXX")"
PROBE_SCRIPT="$(mktemp "${TMPDIR:-/tmp}/semu-fakeroot-probe-script.XXXXXX")"
trap 'rm -rf "$TEST_DIR" "$PROBE_SCRIPT"' EXIT

mkdir -p "$TEST_DIR/src" "$TEST_DIR/dst"
printf 'probe\n' > "$TEST_DIR/src/file"

cat >"$PROBE_SCRIPT" <<'EOF'
#!/bin/sh
set -e

if [ -z "${SEMU_FAKEROOT_TEST_DIR:-}" ] || \
   [ -z "${SEMU_ROOTFS_CP:-}" ] || \
   [ -z "${SEMU_ROOTFS_CHOWN:-}" ]; then
    echo "[!] Missing fakeroot probe environment" >&2
    exit 1
fi

cd "$SEMU_FAKEROOT_TEST_DIR"
"$SEMU_ROOTFS_CP" -a src/. dst/
"$SEMU_ROOTFS_CHOWN" -R 0:0 dst
EOF
chmod +x "$PROBE_SCRIPT"

SEMU_FAKEROOT_TEST_DIR="$TEST_DIR" \
SEMU_ROOTFS_CP="$ROOTFS_CP" \
SEMU_ROOTFS_CHOWN="$ROOTFS_CHOWN" \
fakeroot "$ROOTFS_FAKEROOT_SHELL" "$PROBE_SCRIPT"
