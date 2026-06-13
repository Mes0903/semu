#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "${SCRIPT_DIR}/../common.sh"

KERNEL_DATA="$(scripts/prebuilt/artifact-classes.sh output image)"
INITRD_DATA="$(scripts/prebuilt/artifact-classes.sh output rootfs)"
TEST_TOOLS_DATA="$(scripts/prebuilt/artifact-classes.sh output test-tools)"

export ENABLE_VIRGL="${ENABLE_VIRGL:-1}"
export HEADLESS=0
export DISKIMG_FILE="${DISKIMG_FILE:-${TEST_TOOLS_DATA}}"
export NETDEV="${NETDEV:-user}"
export SMP="${SMP:-2}"
export EXECUTOR="${EXECUTOR:-threaded-cpu-with-device-actors}"

case "${OS_TYPE}" in
    Darwin)
        DEFAULT_BOOT_TIMEOUT=10800
        DEFAULT_CMD_TIMEOUT=600
        DEFAULT_GLXINFO_RETRIES=90
        DEFAULT_XORG_RETRIES=90
        DEFAULT_GLXGEARS_SECONDS=10
        ;;
    *)
        DEFAULT_BOOT_TIMEOUT=300
        DEFAULT_CMD_TIMEOUT=180
        DEFAULT_GLXINFO_RETRIES=45
        DEFAULT_XORG_RETRIES=45
        DEFAULT_GLXGEARS_SECONDS=5
        ;;
esac

export TIMEOUT="${VGPU3D_BOOT_TIMEOUT:-${SEMU_TEST_TIMEOUT:-${DEFAULT_BOOT_TIMEOUT}}}"
export VGPU3D_CMD_TIMEOUT="${VGPU3D_CMD_TIMEOUT:-${DEFAULT_CMD_TIMEOUT}}"
export VGPU3D_GLXINFO_RETRIES="${VGPU3D_GLXINFO_RETRIES:-${DEFAULT_GLXINFO_RETRIES}}"
export VGPU3D_GLXINFO_TIMEOUT="${VGPU3D_GLXINFO_TIMEOUT:-10}"
export VGPU3D_GLXINFO_SLEEP="${VGPU3D_GLXINFO_SLEEP:-1}"
export VGPU3D_XORG_RETRIES="${VGPU3D_XORG_RETRIES:-${DEFAULT_XORG_RETRIES}}"
export VGPU3D_XORG_SLEEP="${VGPU3D_XORG_SLEEP:-1}"
export VGPU3D_GLXGEARS_SECONDS="${VGPU3D_GLXGEARS_SECONDS:-${DEFAULT_GLXGEARS_SECONDS}}"
export VGPU3D_REBOOT_RUNS="${VGPU3D_REBOOT_RUNS:-1}"

case "${VGPU3D_REBOOT_RUNS}" in
    ''|*[!0-9]*)
        print_error "FAIL: VGPU3D_REBOOT_RUNS must be a positive integer"
        exit 1
        ;;
esac
if (( VGPU3D_REBOOT_RUNS < 1 )); then
    print_error "FAIL: VGPU3D_REBOOT_RUNS must be a positive integer"
    exit 1
fi

case "${VGPU3D_GLXGEARS_SECONDS}" in
    ''|*[!0-9]*)
        print_error "FAIL: VGPU3D_GLXGEARS_SECONDS must be a positive integer"
        exit 1
        ;;
esac
if (( VGPU3D_GLXGEARS_SECONDS < 1 )); then
    print_error "FAIL: VGPU3D_GLXGEARS_SECONDS must be a positive integer"
    exit 1
fi

case "${VGPU3D_GLXINFO_TIMEOUT}" in
    ''|*[!0-9]*)
        print_error "FAIL: VGPU3D_GLXINFO_TIMEOUT must be a positive integer"
        exit 1
        ;;
esac
if (( VGPU3D_GLXINFO_TIMEOUT < 1 )); then
    print_error "FAIL: VGPU3D_GLXINFO_TIMEOUT must be a positive integer"
    exit 1
fi

if [ -z "${DISPLAY:-}" ] && [ -z "${WAYLAND_DISPLAY:-}" ]; then
    print_error "FAIL: visible vgpu 3D reboot stress needs host DISPLAY or WAYLAND_DISPLAY"
    exit 1
fi

if ! command -v sdl2-config >/dev/null 2>&1; then
    print_error "FAIL: visible vgpu 3D reboot stress needs sdl2-config in PATH"
    exit 1
fi

case "${ENABLE_VIRGL}" in
    1|true|yes)
        if ! command -v pkg-config >/dev/null 2>&1 ||
           ! pkg-config --exists virglrenderer epoxy gl egl; then
            print_error "FAIL: visible vgpu 3D reboot stress needs pkg-config packages: virglrenderer epoxy gl egl"
            exit 1
        fi
        ;;
esac

cleanup
trap cleanup EXIT

echo "Running vgpu 3D reboot stress: ENABLE_VIRGL=${ENABLE_VIRGL} HEADLESS=${HEADLESS} DISKIMG_FILE=${DISKIMG_FILE} NETDEV=${NETDEV} SMP=${SMP} EXECUTOR=${EXECUTOR} VGPU3D_REBOOT_RUNS=${VGPU3D_REBOOT_RUNS} VGPU3D_GLXGEARS_SECONDS=${VGPU3D_GLXGEARS_SECONDS}"

# Feature toggles are passed through environment variables, which do not
# participate in make's normal dependency tracking. Force a rebuild here so
# visible virgl actor/reboot runs never reuse a stale semu binary or DTB.
make -B semu minimal.dtb

if [ ! -f "${KERNEL_DATA}" ] || [ ! -f "${INITRD_DATA}" ]; then
    make "${KERNEL_DATA}" "${INITRD_DATA}"
fi
if [ ! -f "${TEST_TOOLS_DATA}" ]; then
    make "${TEST_TOOLS_DATA}"
fi
if [[ "${DISKIMG_FILE}" != "${TEST_TOOLS_DATA}" && ! -f "${DISKIMG_FILE}" ]]; then
    print_error "FAIL: DISKIMG_FILE not found: ${DISKIMG_FILE}"
    exit 1
fi

# NOTE: Capture expect's exit code so the shell wrapper can print a concise
# stage-specific message while expect prints detailed guest-side diagnostics.
set +e
expect <<'DONE'
proc send_cmd {cmd exit_code} {
  global timeout
  expect "# " { send -- "$cmd\r" } timeout { exit $exit_code }
}

proc expect_status {ok bad exit_code} {
  global timeout
  expect -exact $ok {} -exact $bad { exit $exit_code } timeout { exit $exit_code }
}

proc send_guest_line {line} {
  send -- $line
  send "\r"
}

proc send_guest_script {script} {
  foreach line [split [string trim $script "\n"] "\n"] {
    send_guest_line $line
  }
}

proc login_guest {exit_code} {
  global env timeout

  set timeout $env(TIMEOUT)
  expect "buildroot login:" { send "root\r" } timeout { exit $exit_code }
  expect "# "              { send "uname -a\r" } timeout { exit 2 }
  expect "riscv32 GNU/Linux" {}
  set timeout $env(VGPU3D_CMD_TIMEOUT)
}

proc run_compact_probe {label} {
  global env timeout

  set timeout $env(VGPU3D_CMD_TIMEOUT)

  puts "--- vgpu 3D compact probe: $label ---"

  send_cmd {ls -la /dev/dri/ 2>/dev/null || true} 3
  send_cmd {if test -c /dev/dri/card0 && test -c /dev/dri/renderD128; then status=OK; else status=MISSING; fi; printf "__VGPU_DRM_%s__\n" "$status"} 3
  expect_status "__VGPU_DRM_OK__" "__VGPU_DRM_MISSING__" 3

  send_cmd {if ls /sys/bus/virtio/drivers/virtio_gpu/virtio* >/dev/null 2>&1; then status=OK; else status=BAD; fi; printf "__VGPU_BIND_%s__\n" "$status"} 3
  expect {
    -exact "__VGPU_BIND_OK__" {}
    -exact "__VGPU_BIND_BAD__" {
      send -- "ls -l /sys/bus/virtio/drivers/virtio_gpu/ 2>/dev/null || true\r"
      send -- "for d in /sys/bus/virtio/devices/virtio*; do echo \$d; ls -l \$d/driver 2>/dev/null || true; done\r"
      exit 3
    }
    timeout { exit 3 }
  }

  send_cmd {dmesg > /tmp/vgpu3d-dmesg.log; grep -Ei 'virtio.*gpu|drm.*virtio|virgl|capset|resource.*blob|host.*visible' /tmp/vgpu3d-dmesg.log | tail -n 120 || true} 4
  send_cmd {status=OK; grep -Fqi '+virgl' /tmp/vgpu3d-dmesg.log || status=BAD; grep -Fqi '+resource_blob' /tmp/vgpu3d-dmesg.log || status=BAD; grep -Fqi '+host_visible' /tmp/vgpu3d-dmesg.log || status=BAD; grep -Eiq 'number of cap sets: *[1-9]|cap set.*id *1' /tmp/vgpu3d-dmesg.log || status=BAD; printf "__VGPU_DMESG_%s__\n" "$status"} 4
  expect_status "__VGPU_DMESG_OK__" "__VGPU_DMESG_BAD__" 4

  send_cmd {if test -f /root/local-env.sh; then status=OK; else status=MISSING; fi; printf "__LOCALENV_%s__\n" "$status"} 5
  expect_status "__LOCALENV_OK__" "__LOCALENV_MISSING__" 5

  send_cmd {. /root/local-env.sh >/dev/null 2>&1; if test $? -eq 0; then status=OK; else status=FAIL; fi; export DISPLAY=:0; printf "__LOCALENV_SRC_%s__\n" "$status"} 5
  expect_status "__LOCALENV_SRC_OK__" "__LOCALENV_SRC_FAIL__" 5

  send_cmd {ls -l /usr/lib/dri 2>/dev/null || true; if test -f /etc/semu-test-tools-virgl && test -e /usr/lib/dri/virtio_gpu_dri.so; then status=OK; else status=MISSING; fi; printf "__VIRGL_GUEST_DRIVER_%s__\n" "$status"} 5
  expect_status "__VIRGL_GUEST_DRIVER_OK__" "__VIRGL_GUEST_DRIVER_MISSING__" 5

  send_cmd {if command -v glxinfo >/dev/null 2>&1 && command -v glxgears >/dev/null 2>&1; then status=OK; else status=MISSING; fi; printf "__VIRGL_APPS_%s__\n" "$status"} 5
  expect_status "__VIRGL_APPS_OK__" "__VIRGL_APPS_MISSING__" 5

  send_cmd {if test -S /tmp/.X11-unix/X0; then status=RUNNING; elif command -v Xorg >/dev/null 2>&1; then rm -f /tmp/.X0-lock; Xorg :0 -noreset -nolisten tcp >/tmp/xorg.log 2>&1 & echo $! >/tmp/xorg.pid; status=STARTED; else status=MISSING; fi; printf "__VIRGL_XORG_%s__\n" "$status"} 6
  expect {
    -exact "__VIRGL_XORG_RUNNING__" {}
    -exact "__VIRGL_XORG_STARTED__" {}
    -exact "__VIRGL_XORG_MISSING__" { exit 6 }
    timeout { exit 6 }
  }

  set xorg_cmd "i=0; status=FAIL; while test \$i -lt $env(VGPU3D_XORG_RETRIES); do if test -S /tmp/.X11-unix/X0; then status=READY; break; fi; sleep $env(VGPU3D_XORG_SLEEP); i=\$((i + 1)); done; printf \"__VIRGL_XORG_%s__\\n\" \"\$status\""
  send_cmd $xorg_cmd 6
  expect {
    -exact "__VIRGL_XORG_READY__" {}
    -exact "__VIRGL_XORG_FAIL__" {
      send -- "cat /tmp/xorg.log 2>/dev/null || true\r"
      exit 6
    }
    timeout { exit 6 }
  }

  expect "# " {
    send_guest_line {cat > /tmp/vgpu3d-compact-probe.sh <<'VGPU3D_COMPACT'}
    send_guest_line {#!/bin/sh}
    send_guest_line "glxinfo_retries=$env(VGPU3D_GLXINFO_RETRIES)"
    send_guest_line "glxinfo_timeout=$env(VGPU3D_GLXINFO_TIMEOUT)"
    send_guest_line "glxinfo_sleep=$env(VGPU3D_GLXINFO_SLEEP)"
    send_guest_line "glxgears_seconds=$env(VGPU3D_GLXGEARS_SECONDS)"
    send_guest_script {
status=OK

print_xorg_logs() {
  for log_file in /tmp/xorg.log /var/log/Xorg.0.0.log /var/log/Xorg.0.log; do
    if test -f "$log_file"; then
      echo "--- xorg log tail ($log_file) ---"
      tail -120 "$log_file" 2>/dev/null || true
    fi
  done
}

run_glxinfo_bounded() {
  log_file=$1
  forced=$2
  timeout_message=$3
  timeout_flag=$4

  glxinfo_rc=1
  rm -f "$timeout_flag"
  if test "$forced" = 1; then
    DISPLAY=:0 LIBGL_DEBUG=verbose MESA_LOADER_DRIVER_OVERRIDE=virtio_gpu glxinfo -B >"$log_file" 2>&1 &
  else
    DISPLAY=:0 glxinfo -B >"$log_file" 2>&1 &
  fi
  glxinfo_pid=$!
  (
    sleep "$glxinfo_timeout"
    if kill -0 "$glxinfo_pid" 2>/dev/null; then
      : >"$timeout_flag"
      kill "$glxinfo_pid" 2>/dev/null || true
      sleep 1
      kill -KILL "$glxinfo_pid" 2>/dev/null || true
    fi
  ) &
  glxinfo_watchdog=$!
  wait "$glxinfo_pid"
  glxinfo_rc=$?
  kill "$glxinfo_watchdog" 2>/dev/null || true
  wait "$glxinfo_watchdog" 2>/dev/null || true
  if test -f "$timeout_flag"; then
    glxinfo_rc=124
  fi

  if test "$glxinfo_rc" -eq 124; then
    echo "$timeout_message" >>"$log_file"
  fi
  return "$glxinfo_rc"
}

run_glxgears_bounded() {
  log_file=$1
  timeout_flag=$2

  rm -f "$timeout_flag"
  DISPLAY=:0 glxgears -info >"$log_file" 2>&1 &
  glxgears_pid=$!
  (
    sleep "$glxgears_seconds"
    if kill -0 "$glxgears_pid" 2>/dev/null; then
      : >"$timeout_flag"
      kill "$glxgears_pid" 2>/dev/null || true
      sleep 1
      kill -KILL "$glxgears_pid" 2>/dev/null || true
    fi
  ) &
  glxgears_watchdog=$!
  wait "$glxgears_pid"
  glxgears_rc=$?
  kill "$glxgears_watchdog" 2>/dev/null || true
  wait "$glxgears_watchdog" 2>/dev/null || true

  if test -f "$timeout_flag"; then
    echo "glxgears stayed alive for ${glxgears_seconds}s" >>"$log_file"
    glxgears_rc=124
  fi
  return "$glxgears_rc"
}

echo "--- compact glxinfo probe ---"
rm -f /tmp/vgpu3d-glxinfo.log
i=0
glxinfo_status=FAIL
while test "$i" -lt "$glxinfo_retries"; do
  run_glxinfo_bounded /tmp/vgpu3d-glxinfo.log 0 \
    "glxinfo -B timed out after ${glxinfo_timeout}s" \
    /tmp/vgpu3d-glxinfo-timeout.flag
  glxinfo_rc=$?
  if test "$glxinfo_rc" -eq 0; then
    glxinfo_status=OK
    break
  fi
  if test "$glxinfo_rc" -eq 124; then
    glxinfo_status=TIMEOUT
    break
  fi
  i=$((i + 1))
  sleep "$glxinfo_sleep"
done

head -80 /tmp/vgpu3d-glxinfo.log
if test "$glxinfo_status" != OK ||
   ! grep -Eiq 'OpenGL renderer string:.*virgl|Device:.*virgl|virgl' /tmp/vgpu3d-glxinfo.log; then
  echo '--- forced virgl loader diagnostic ---'
  run_glxinfo_bounded /tmp/vgpu3d-glxinfo-virtio.log 1 \
    "forced glxinfo -B timed out after ${glxinfo_timeout}s" \
    /tmp/vgpu3d-glxinfo-virtio-timeout.flag
  head -120 /tmp/vgpu3d-glxinfo-virtio.log
  print_xorg_logs
  echo '--- dmesg virgl tail ---'
  dmesg | grep -Ei 'virtio.*gpu|drm.*virtio|virgl|capset|resource.*blob|host.*visible' | tail -120 || true
  status=GLXINFO
fi

if test "$status" = OK; then
  echo "--- compact glxgears probe ---"
  rm -f /tmp/vgpu3d-glxgears.log
  run_glxgears_bounded /tmp/vgpu3d-glxgears.log /tmp/vgpu3d-glxgears-timeout.flag
  glxgears_rc=$?
  head -40 /tmp/vgpu3d-glxgears.log
  if test "$glxgears_rc" -ne 124; then
    print_xorg_logs
    echo '--- dmesg virgl tail ---'
    dmesg | grep -Ei 'virtio.*gpu|drm.*virtio|virgl|capset|resource.*blob|host.*visible' | tail -120 || true
    status=GLXGEARS
  fi
fi

printf "__VGPU_COMPACT_%s__\n" "$status"
}
    send_guest_line {VGPU3D_COMPACT}
    send_guest_line {chmod +x /tmp/vgpu3d-compact-probe.sh}
    send_guest_line {/tmp/vgpu3d-compact-probe.sh}
  } timeout { exit 7 }
  expect {
    -exact "__VGPU_COMPACT_OK__" {}
    -exact "__VGPU_COMPACT_GLXINFO__" { exit 7 }
    -exact "__VGPU_COMPACT_GLXGEARS__" { exit 8 }
    timeout { exit 8 }
  }
}

proc boot_and_probe {label login_exit_code} {
  global env timeout spawn_id

  set timeout $env(TIMEOUT)
  spawn make check DISKIMG_FILE=$env(DISKIMG_FILE)

  login_guest $login_exit_code
  run_compact_probe $label
}

proc request_guest_reboot {exit_code} {
  global env timeout spawn_id

  expect "# " { send "sync; reboot -f\r" } timeout { exit 9 }

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

for {set run 1} {$run <= $env(VGPU3D_REBOOT_RUNS)} {incr run} {
  puts "--- vgpu 3D reboot cycle $run/$env(VGPU3D_REBOOT_RUNS) ---"
  request_guest_reboot 10
  boot_and_probe "after reboot $run" 10
}
DONE

ret="$?"
set -e

MESSAGES=(
  "PASS: visible vgpu 3D reboot stress"
  "FAIL: initial boot/login prompt not found"
  "FAIL: shell prompt not found"
  "FAIL: virtio-gpu basic checks failed (/dev/dri/card0, /dev/dri/renderD128, or virtio_gpu binding)"
  "FAIL: virgl/capset/resource_blob/host_visible dmesg checks failed"
  "FAIL: guest VirGL tools/driver missing or local-env.sh failed"
  "FAIL: guest Xorg did not start on :0"
  "FAIL: glxinfo -B failed or did not report a virgl renderer"
  "FAIL: glxgears did not start cleanly"
  "FAIL: guest reboot command was not sent"
  "FAIL: guest reboot did not produce a clean semu reset/exit or the next boot did not reach login"
)

if [[ "${ret}" -eq 0 ]]; then
  print_success "${MESSAGES[0]}"
  exit 0
fi

print_error "${MESSAGES[${ret}]:-FAIL: unknown error (exit code ${ret})}"
exit "${ret}"
