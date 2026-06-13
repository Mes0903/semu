#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "${SCRIPT_DIR}/../common.sh"

cleanup
trap cleanup EXIT

SMP="${SMP:-2}"
if ! [[ "${SMP}" =~ ^[0-9]+$ ]] || ((SMP < 2)); then
    print_error "FAIL: HSM CPU hotplug checks require SMP >= 2"
    exit 3
fi
export SMP

# Feature toggles are passed through environment variables, which do not
# participate in normal dependency tracking by 'make'. Force a rebuild here so
# hotplug smoke never reuses a stale binary or DTB.
make -B semu minimal.dtb

if [ ! -f Image ] || [ ! -f rootfs.cpio ]; then
    make Image rootfs.cpio
fi
if [ ! -f ext4.img ]; then
    make ext4.img
fi

set +e
expect <<'DONE'
set timeout $env(TIMEOUT)
spawn make check

expect "buildroot login:" { send "root\r" } timeout { exit 1 }
expect "# "              { send "uname -a\r" } timeout { exit 2 }
expect "riscv32 GNU/Linux" {}

expect "# " {
  send "sh -lc 'if test -w /sys/devices/system/cpu/cpu1/online; then status=OK; else status=MISSING; fi; printf \"__HSM_CPU1_%s__\\n\" \"\u0024status\"'\r"
} timeout { exit 3 }
expect {
  -exact "__HSM_CPU1_OK__" {}
  -exact "__HSM_CPU1_MISSING__" { exit 3 }
  timeout { exit 3 }
}

expect "# " {
  send "sh -lc 'echo 0 > /sys/devices/system/cpu/cpu1/online && v=\u0024(cat /sys/devices/system/cpu/cpu1/online); printf \"__HSM_OFF_%s__\\n\" \"\u0024v\"'\r"
} timeout { exit 4 }
expect {
  -exact "__HSM_OFF_0__" {}
  timeout { exit 4 }
}

expect "# " {
  send "sh -lc 'echo 1 > /sys/devices/system/cpu/cpu1/online && v=\u0024(cat /sys/devices/system/cpu/cpu1/online); printf \"__HSM_ON_%s__\\n\" \"\u0024v\"'\r"
} timeout { exit 5 }
expect {
  -exact "__HSM_ON_1__" {}
  timeout { exit 5 }
}
DONE

ret="$?"
set -e

if [[ "${ret}" -eq 0 ]]; then
    print_success "PASS: HSM CPU hotplug checks"
    exit 0
fi

MESSAGES=(
    "unused"
    "FAIL: boot/login prompt not found"
    "FAIL: shell prompt not found"
    "FAIL: cpu1 online control missing"
    "FAIL: cpu1 offline transition failed"
    "FAIL: cpu1 online transition failed"
)

print_error "${MESSAGES[${ret}]:-FAIL: unknown error (exit code ${ret})}"
exit "${ret}"
