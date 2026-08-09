#!/usr/bin/env bash
#
# Build an ext4 rootfs image from an existing cpio archive.
#
# Usage: rootfs_ext4.sh [SOURCE_CPIO] [OUT_IMG] [SIZE_MB] [EXTRA_DIR]
#
# Default values match the EXTROOT make path: read rootfs.cpio, produce
# ext4.img sized at 32 MiB. The 32 MiB default fits the buildroot userland
# with headroom; bump SIZE_MB for larger rootfs payloads. EXTRA_DIR, when
# given, is copied into the ext4 image after SOURCE_CPIO is extracted without
# changing SOURCE_CPIO itself.

set -euo pipefail

SRC_CPIO="${1:-rootfs.cpio}"
OUT_IMG="${2:-ext4.img}"
SIZE_MB="${3:-32}"
EXTRA_DIR="${4:-}"
MKFS_EXT4="${MKFS_EXT4:-mkfs.ext4}"
# Caller-facing hooks for hosts where the default toolchain is unsuitable.
# CI can point these at a package-manager-provided toolchain without baking
# platform policy into this helper.
ROOTFS_CPIO="${ROOTFS_CPIO:-cpio}"
ROOTFS_CP="${ROOTFS_CP:-cp}"
ROOTFS_CHOWN="${ROOTFS_CHOWN:-chown}"
ROOTFS_FAKEROOT_SHELL="${ROOTFS_FAKEROOT_SHELL:-/bin/sh}"

if [ ! -f "$SRC_CPIO" ]; then
    echo "[!] Source cpio not found: $SRC_CPIO" >&2
    exit 1
fi

if ! command -v fakeroot >/dev/null 2>&1; then
    echo "[!] fakeroot is required to build the ext4 image" >&2
    exit 1
fi

if ! [ -x "$ROOTFS_CPIO" ] && ! command -v "$ROOTFS_CPIO" >/dev/null 2>&1; then
    echo "[!] cpio is required to build the ext4 image" >&2
    exit 1
fi

if ! [ -x "$ROOTFS_CP" ] && ! command -v "$ROOTFS_CP" >/dev/null 2>&1; then
    echo "[!] cp is required to build the ext4 image" >&2
    exit 1
fi

if ! [ -x "$ROOTFS_CHOWN" ] && ! command -v "$ROOTFS_CHOWN" >/dev/null 2>&1; then
    echo "[!] chown is required to build the ext4 image" >&2
    exit 1
fi

if ! [ -x "$ROOTFS_FAKEROOT_SHELL" ] && ! command -v "$ROOTFS_FAKEROOT_SHELL" >/dev/null 2>&1; then
    echo "[!] fakeroot shell is required to build the ext4 image" >&2
    exit 1
fi

if ! [ -x "$MKFS_EXT4" ] && ! command -v "$MKFS_EXT4" >/dev/null 2>&1; then
    echo "[!] mkfs.ext4 is required to build the ext4 image" >&2
    exit 1
fi

SRC_DIR="$(cd "$(dirname "$SRC_CPIO")" && pwd -P)"
SRC_ABS="$SRC_DIR/$(basename "$SRC_CPIO")"
OUT_DIR="$(cd "$(dirname "$OUT_IMG")" && pwd -P)"
OUT_ABS="$OUT_DIR/$(basename "$OUT_IMG")"
EXTRA_ABS=""
if [ -n "$EXTRA_DIR" ]; then
    if [ ! -d "$EXTRA_DIR" ]; then
        echo "[!] Extra directory not found: $EXTRA_DIR" >&2
        exit 1
    fi
    EXTRA_ABS="$(cd "$EXTRA_DIR" && pwd -P)"
fi
# `mktemp -d -t PREFIX` differs between GNU (PREFIX is a name) and BSD (PREFIX
# is a template) -- spell out the full template instead.
STAGE="$(mktemp -d "${TMPDIR:-/tmp}/semu-rootfs.XXXXXX")"
OUT_TMP="$(mktemp "$OUT_DIR/.$(basename "$OUT_IMG").XXXXXX")"
FAKEROOT_SCRIPT="$(mktemp "${TMPDIR:-/tmp}/semu-fakeroot.XXXXXX")"
trap 'rm -rf "$STAGE" "$OUT_TMP" "$FAKEROOT_SCRIPT"' EXIT

echo "[*] Creating empty image: $OUT_IMG (${SIZE_MB} MiB)"
# bs=1024k works on both GNU and BSD dd; bs=1M is GNU-only and bs=1m is
# BSD-only.
dd if=/dev/zero of="$OUT_TMP" bs=1024k count="$SIZE_MB" >/dev/null 2>&1

echo "[*] Building ext4 filesystem"
echo "[*] Extracting $SRC_CPIO -> $STAGE"
if [ -n "$EXTRA_ABS" ]; then
    echo "[*] Applying extra files: $EXTRA_DIR"
fi

# -E lazy_*_init=0: do all init at mkfs time so the first guest mount does
# not pay the lazy-init cost. Stripping the journal (-O ^has_journal)
# would also speed mount, but the prebuilt Linux Image is built with
# CONFIG_EXT4_USE_FOR_EXT2=n and refuses to mount a no-journal image.
#
# Keep cpio extraction and mkfs in the same fakeroot child so ownership and
# device-node metadata are preserved in the generated image. Run a temporary
# script through an explicit shell so callers can choose a shell from the same
# toolchain as the other fakeroot child commands.
cat >"$FAKEROOT_SCRIPT" <<'EOF'
#!/bin/sh
set -e

if [ -z "${SEMU_ROOTFS_STAGE:-}" ] || \
   [ -z "${SEMU_SRC_CPIO:-}" ] || \
   [ -z "${SEMU_ROOTFS_CPIO:-}" ] || \
   [ -z "${SEMU_ROOTFS_CP:-}" ] || \
   [ -z "${SEMU_ROOTFS_CHOWN:-}" ] || \
   [ -z "${SEMU_MKFS_EXT4:-}" ] || \
   [ -z "${SEMU_OUT_IMG:-}" ]; then
    echo "[!] Missing rootfs_ext4.sh fakeroot environment" >&2
    exit 1
fi

cd "$SEMU_ROOTFS_STAGE"
set +e
"$SEMU_ROOTFS_CPIO" -idm < "$SEMU_SRC_CPIO"
cpio_status=$?
set -e
if [ ! -x sbin/init ]; then
    echo "[!] Extracted rootfs does not contain executable /sbin/init" >&2
    exit 1
fi
if [ "$cpio_status" -ne 0 ]; then
    echo "[*] cpio reported non-fatal extraction warnings; continuing after rootfs validation" >&2
fi
if [ -n "$SEMU_EXTRA_DIR" ]; then
    "$SEMU_ROOTFS_CP" -a "$SEMU_EXTRA_DIR"/. .
fi
# Preserve the historical ext4.img policy: generated images are fully
# root-owned after cpio extraction and any optional overlay copy.
"$SEMU_ROOTFS_CHOWN" -R 0:0 .
"$SEMU_MKFS_EXT4" -q -F \
    -E lazy_itable_init=0,lazy_journal_init=0 \
    -d . "$SEMU_OUT_IMG"
EOF
chmod +x "$FAKEROOT_SCRIPT"

SEMU_ROOTFS_STAGE="$STAGE" \
SEMU_SRC_CPIO="$SRC_ABS" \
SEMU_ROOTFS_CPIO="$ROOTFS_CPIO" \
SEMU_ROOTFS_CP="$ROOTFS_CP" \
SEMU_ROOTFS_CHOWN="$ROOTFS_CHOWN" \
SEMU_EXTRA_DIR="$EXTRA_ABS" \
SEMU_MKFS_EXT4="$MKFS_EXT4" \
SEMU_OUT_IMG="$OUT_TMP" \
fakeroot "$ROOTFS_FAKEROOT_SHELL" "$FAKEROOT_SCRIPT"

mv -f "$OUT_TMP" "$OUT_ABS"
du -h "$OUT_ABS"
