#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "${SCRIPT_DIR}/../common.sh"

KERNEL_DATA="$(scripts/prebuilt/artifact-classes.sh output image)"
INITRD_DATA="$(scripts/prebuilt/artifact-classes.sh output rootfs)"

: "${EXECUTOR=threaded-cpu-with-device-actors}"
export EXECUTOR
: "${SMP=2}"
export SMP
: "${NETDEV=user}"
export NETDEV
: "${SEMU_ACTOR_CROSS_REBOOT_RUNS=2}"
export SEMU_ACTOR_CROSS_REBOOT_RUNS

case "${OS_TYPE}" in
    Darwin)
        DEFAULT_BOOT_TIMEOUT=1800
        DEFAULT_CMD_TIMEOUT=600
        ;;
    *)
        DEFAULT_BOOT_TIMEOUT=180
        DEFAULT_CMD_TIMEOUT=90
        ;;
esac

: "${SEMU_ACTOR_CROSS_REBOOT_TIMEOUT=${DEFAULT_BOOT_TIMEOUT}}"
export TIMEOUT="${SEMU_ACTOR_CROSS_REBOOT_TIMEOUT}"
: "${SEMU_ACTOR_CROSS_REBOOT_CMD_TIMEOUT=${DEFAULT_CMD_TIMEOUT}}"
export SEMU_ACTOR_CROSS_REBOOT_CMD_TIMEOUT
: "${SEMU_ACTOR_CROSS_SOUND_SAMPLE=/usr/share/sounds/alsa/Front_Center.wav}"
export SEMU_ACTOR_CROSS_SOUND_SAMPLE
: "${SEMU_ACTOR_CROSS_REBOOT_EXPECT_LOG:=}"
export SEMU_ACTOR_CROSS_REBOOT_EXPECT_LOG

case "${SEMU_ACTOR_CROSS_REBOOT_RUNS}" in
    ''|*[!0-9]*)
        echo "FAIL: SEMU_ACTOR_CROSS_REBOOT_RUNS must be a positive integer" >&2
        exit 1
        ;;
esac
if (( 10#${SEMU_ACTOR_CROSS_REBOOT_RUNS} < 1 )); then
    echo "FAIL: SEMU_ACTOR_CROSS_REBOOT_RUNS must be a positive integer" >&2
    exit 1
fi

if [[ -z "${EXECUTOR}" ]]; then
    echo "FAIL: EXECUTOR must be non-empty" >&2
    exit 1
fi
if [[ -z "${SMP}" ]]; then
    echo "FAIL: SMP must be non-empty" >&2
    exit 1
fi
if [[ -z "${NETDEV}" ]]; then
    echo "FAIL: NETDEV must be non-empty" >&2
    exit 1
fi
if [[ "${NETDEV}" != "user" ]]; then
    echo "FAIL: all-reboot requires NETDEV=user" >&2
    exit 1
fi

if [[ "${SEMU_ACTOR_CROSS_REBOOT_PRINT_CONFIG:-0}" == 1 ]]; then
    printf 'SEMU_ACTOR_CROSS_REBOOT_RUNS=%s\n' "${SEMU_ACTOR_CROSS_REBOOT_RUNS}"
    printf 'EXECUTOR=%s\n' "${EXECUTOR}"
    printf 'SMP=%s\n' "${SMP}"
    printf 'NETDEV=%s\n' "${NETDEV}"
    printf 'TIMEOUT=%s\n' "${TIMEOUT}"
    printf 'SEMU_ACTOR_CROSS_REBOOT_CMD_TIMEOUT=%s\n' "${SEMU_ACTOR_CROSS_REBOOT_CMD_TIMEOUT}"
    printf 'SEMU_ACTOR_CROSS_SOUND_SAMPLE=%s\n' "${SEMU_ACTOR_CROSS_SOUND_SAMPLE}"
    printf 'SEMU_ACTOR_CROSS_REBOOT_EXPECT_LOG=%s\n' "${SEMU_ACTOR_CROSS_REBOOT_EXPECT_LOG}"
    exit 0
fi

cleanup
trap cleanup EXIT

make -B semu minimal.dtb

if [ ! -f "${KERNEL_DATA}" ] || [ ! -f "${INITRD_DATA}" ]; then
    make "${KERNEL_DATA}" "${INITRD_DATA}"
fi

echo "Running full actor cross-device reboot stress: EXECUTOR=${EXECUTOR} SMP=${SMP} NETDEV=${NETDEV} SEMU_ACTOR_CROSS_REBOOT_RUNS=${SEMU_ACTOR_CROSS_REBOOT_RUNS}"

set +e
expect <<'DONE'
log_user 0
if {[info exists env(SEMU_ACTOR_CROSS_REBOOT_EXPECT_LOG)] && $env(SEMU_ACTOR_CROSS_REBOOT_EXPECT_LOG) ne ""} {
  log_file -a $env(SEMU_ACTOR_CROSS_REBOOT_EXPECT_LOG)
}

proc exit_after_child_closed {exit_code message} {
  puts stderr $message
  catch {
    set wait_status [wait]
    puts stderr "child_wait_status=$wait_status"
  }
  exit $exit_code
}

proc send_cmd {cmd exit_code} {
  global spawn_id timeout
  expect "# " { send -- "$cmd\r" } timeout { exit $exit_code }
}

proc expect_marker {ok bad exit_code} {
  global spawn_id timeout
  expect {
    -exact $ok {}
    -exact $bad { exit $exit_code }
    timeout { exit $exit_code }
    eof { exit $exit_code }
  }
}

proc login_guest {exit_code} {
  global env spawn_id timeout

  set timeout $env(TIMEOUT)
  expect {
    "buildroot login:" { send "root\r" }
    timeout { puts stderr "timeout waiting for buildroot login"; exit $exit_code }
    eof { exit_after_child_closed $exit_code "eof waiting for buildroot login" }
  }
  expect {
    "# " { send "uname -a\r" }
    timeout { puts stderr "timeout waiting for shell prompt"; exit 2 }
    eof { exit_after_child_closed 2 "eof waiting for shell prompt" }
  }
  expect {
    "riscv32 GNU/Linux" {}
    timeout { puts stderr "timeout waiting for uname evidence"; exit 2 }
    eof { exit_after_child_closed 2 "eof waiting for uname evidence" }
  }
  set timeout $env(SEMU_ACTOR_CROSS_REBOOT_CMD_TIMEOUT)
}

proc expect_strict_ping {target exit_code} {
  global spawn_id timeout

  expect "# " {} timeout { exit $exit_code }
  for {set attempt 1} {$attempt <= 5} {incr attempt} {
    send -- "echo NETDEV_STRICT_PING_ATTEMPT:$attempt\r"
    expect "# " { send -- "ping -c 3 -W 5 $target\r" } timeout { exit $exit_code }
    expect {
      "3 packets transmitted, 3 packets received, 0% packet loss" {
        return
      }
      -re {3 packets transmitted, [0-9]+ packets received, [0-9]+% packet loss} {
        expect "# " {} timeout { exit $exit_code }
      }
      timeout { exit $exit_code }
      eof { exit $exit_code }
    }
  }
  send -- "echo NETDEV_STRICT_PING_FAIL\r"
  exit $exit_code
}

proc run_probe {label} {
  global env spawn_id timeout

  puts "--- full actor cross-device probe: $label ---"

  send_cmd {ls -la /dev/dri/ 2>/dev/null || true} 3
  send_cmd {if test -c /dev/dri/card0; then status=OK; else status=MISSING; fi; printf "__ACTOR_GPU_DRM_%s__\n" "$status"} 3
  expect_marker "__ACTOR_GPU_DRM_OK__" "__ACTOR_GPU_DRM_MISSING__" 3

  send_cmd {if ls /sys/bus/virtio/drivers/virtio_gpu/virtio* >/dev/null 2>&1; then status=OK; else status=BAD; fi; printf "__ACTOR_GPU_BIND_%s__\n" "$status"} 3
  expect_marker "__ACTOR_GPU_BIND_OK__" "__ACTOR_GPU_BIND_BAD__" 3

  send_cmd {if ls /dev/input/event* >/dev/null 2>&1; then status=OK; else status=BAD; fi; printf "__ACTOR_INPUT_EVT_%s__\n" "$status"} 4
  expect_marker "__ACTOR_INPUT_EVT_OK__" "__ACTOR_INPUT_EVT_BAD__" 4

  send_cmd {cat /proc/bus/input/devices | head -20} 4
  send_cmd {if grep -qi virtio /proc/bus/input/devices; then status=OK; else status=BAD; fi; printf "__ACTOR_INPUT_PROC_%s__\n" "$status"} 4
  expect_marker "__ACTOR_INPUT_PROC_OK__" "__ACTOR_INPUT_PROC_BAD__" 4

  send_cmd {ip addr add 10.0.2.15/24 dev eth0 2>/dev/null || true} 5
  send_cmd {ip link set eth0 up} 5
  send_cmd {ip route add default via 10.0.2.2 2>/dev/null || ip route replace default via 10.0.2.2} 5
  expect_strict_ping "10.0.2.2" 5

  send_cmd "aplay $env(SEMU_ACTOR_CROSS_SOUND_SAMPLE)" 6
  expect {
    " Mono" {}
    timeout { exit 7 }
    eof { exit 7 }
  }
}

proc boot_and_probe {label login_exit_code} {
  global env spawn_id timeout

  set timeout $env(TIMEOUT)
  spawn make check NETDEV=$env(NETDEV) EXECUTOR=$env(EXECUTOR) SMP=$env(SMP)
  login_guest $login_exit_code
  run_probe $label
}

proc request_guest_reboot {exit_code} {
  global env spawn_id timeout

  expect "# " { send "sync; reboot -f\r" } timeout { exit 8 }

  set saw_reset 0
  set timeout $env(TIMEOUT)
  expect {
    -re {system reset: type=[0-9]+, reason=[0-9]+} {
      set saw_reset 1
      exp_continue
    }
    eof {
      if {!$saw_reset} {
        exit $exit_code
      }
      set wait_status [wait]
      set child_rc [lindex $wait_status 3]
      if {$child_rc != 0} {
        exit $exit_code
      }
    }
    timeout { exit $exit_code }
  }
}

boot_and_probe "initial" 1

for {set run 1} {$run <= $env(SEMU_ACTOR_CROSS_REBOOT_RUNS)} {incr run} {
  puts "--- full actor cross-device reboot cycle $run/$env(SEMU_ACTOR_CROSS_REBOOT_RUNS) ---"
  request_guest_reboot 9
  boot_and_probe "after reboot $run" 9
}
DONE

ret="$?"
set -e

MESSAGES=(
  "PASS: headless full actor GPU/input/net/sound reboot stress"
  "FAIL: boot/login prompt not found"
  "FAIL: shell prompt not found"
  "FAIL: headless virtio-gpu basic checks failed"
  "FAIL: virtio-input basic checks failed"
  "FAIL: user-mode netdev strict ping failed"
  "FAIL: sound playback command failed"
  "FAIL: sound playback output missing Mono evidence"
  "FAIL: guest reboot command did not reach shell prompt"
  "FAIL: guest reboot/system-reset boundary failed"
)

if [[ "${ret}" -eq 0 ]]; then
  print_success "${MESSAGES[0]}"
  exit 0
fi

print_error "${MESSAGES[${ret}]:-FAIL: unknown error (exit code ${ret})}"
exit "${ret}"
