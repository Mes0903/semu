#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "${TMP_DIR}"' EXIT

fail_pkg_config="${TMP_DIR}/pkg-config-fail"
fake_pkg_config="${TMP_DIR}/pkg-config-fake"

cat >"${fail_pkg_config}" <<'SCRIPT'
#!/usr/bin/env bash
exit 1
SCRIPT
chmod +x "${fail_pkg_config}"

cat >"${fake_pkg_config}" <<'SCRIPT'
#!/usr/bin/env bash
case "$1" in
    --exists)
        exit 0
        ;;
    --cflags)
        printf '%s\n' '-DFAKE_VIRGL_CFLAGS -I/fake/include'
        ;;
    --libs)
        printf '%s\n' '-L/fake/lib -lvirglrenderer -lepoxy -lGL -lEGL'
        ;;
    *)
        exit 2
        ;;
esac
SCRIPT
chmod +x "${fake_pkg_config}"

disabled_output="$(
    make -s -C "${REPO_ROOT}" ENABLE_VIRGL=0 PKG_CONFIG="${fail_pkg_config}" \
        print-vgpu-virgl-config
)"
grep -qx 'SEMU_FEATURE_VIRGL=0' <<<"${disabled_output}"
grep -qx 'VIRGL_CFLAGS=' <<<"${disabled_output}"
grep -qx 'VIRGL_LIBS=' <<<"${disabled_output}"

if missing_output="$(
    make -s -C "${REPO_ROOT}" ENABLE_VIRGL=1 PKG_CONFIG="${fail_pkg_config}" \
        print-vgpu-virgl-config 2>&1
)"; then
    printf '%s\n' 'expected ENABLE_VIRGL=1 with missing deps to fail'
    exit 1
fi
grep -q 'ENABLE_VIRGL=1 requires pkg-config packages: virglrenderer epoxy gl egl' \
    <<<"${missing_output}"

if gpu_disabled_output="$(
    make -s -C "${REPO_ROOT}" ENABLE_VIRTIOGPU=0 ENABLE_VIRGL=1 \
        PKG_CONFIG="${fake_pkg_config}" print-vgpu-virgl-config 2>&1
)"; then
    printf '%s\n' 'expected ENABLE_VIRGL=1 with ENABLE_VIRTIOGPU=0 to fail'
    exit 1
fi
grep -q 'ENABLE_VIRGL=1 requires ENABLE_VIRTIOGPU=1' <<<"${gpu_disabled_output}"

enabled_output="$(
    make -s -C "${REPO_ROOT}" ENABLE_VIRGL=1 PKG_CONFIG="${fake_pkg_config}" \
        print-vgpu-virgl-config
)"
grep -qx 'SEMU_FEATURE_VIRGL=1' <<<"${enabled_output}"
grep -qx 'VIRGL_PKGS=virglrenderer epoxy gl egl' <<<"${enabled_output}"
grep -qx 'VIRGL_CFLAGS=-DFAKE_VIRGL_CFLAGS -I/fake/include' <<<"${enabled_output}"
grep -qx 'VIRGL_LIBS=-L/fake/lib -lvirglrenderer -lepoxy -lGL -lEGL' \
    <<<"${enabled_output}"
