#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "${SCRIPT_DIR}/../common.sh"

KERNEL_DATA="$(scripts/prebuilt/artifact-classes.sh output image)"
INITRD_DATA="$(scripts/prebuilt/artifact-classes.sh output rootfs)"
TEST_TOOLS_DATA="$(scripts/prebuilt/artifact-classes.sh output test-tools)"

export ENABLE_VIRGL="${ENABLE_VIRGL:-1}"
export HEADLESS="${HEADLESS:-0}"
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
export VGPU3D_GLXGEARS_RUNS="${VGPU3D_GLXGEARS_RUNS:-1}"
export VGPU3D_XORG_RESTARTS="${VGPU3D_XORG_RESTARTS:-0}"
export VGPU3D_STRESS_GLXGEARS_SECONDS="${VGPU3D_STRESS_GLXGEARS_SECONDS:-${VGPU3D_GLXGEARS_SECONDS}}"
export VGPU3D_EXPECT_WINDOW_CLOSE="${VGPU3D_EXPECT_WINDOW_CLOSE:-0}"
export VGPU3D_WINDOW_CLOSE_WAIT_TIMEOUT="${VGPU3D_WINDOW_CLOSE_WAIT_TIMEOUT:-30}"

case "${VGPU3D_GLXGEARS_RUNS}" in
    ''|*[!0-9]*)
        print_error "FAIL: VGPU3D_GLXGEARS_RUNS must be a positive integer"
        exit 1
        ;;
esac
if (( VGPU3D_GLXGEARS_RUNS < 1 )); then
    print_error "FAIL: VGPU3D_GLXGEARS_RUNS must be a positive integer"
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

case "${VGPU3D_XORG_RESTARTS}" in
    ''|*[!0-9]*)
        print_error "FAIL: VGPU3D_XORG_RESTARTS must be a non-negative integer"
        exit 1
        ;;
esac

case "${VGPU3D_STRESS_GLXGEARS_SECONDS}" in
    ''|*[!0-9]*)
        print_error "FAIL: VGPU3D_STRESS_GLXGEARS_SECONDS must be a positive integer"
        exit 1
        ;;
esac
if (( VGPU3D_STRESS_GLXGEARS_SECONDS < 1 )); then
    print_error "FAIL: VGPU3D_STRESS_GLXGEARS_SECONDS must be a positive integer"
    exit 1
fi

case "${VGPU3D_CMD_TIMEOUT}" in
    ''|*[!0-9]*)
        print_error "FAIL: VGPU3D_CMD_TIMEOUT must be a positive integer"
        exit 1
        ;;
esac
if (( VGPU3D_CMD_TIMEOUT < 1 )); then
    print_error "FAIL: VGPU3D_CMD_TIMEOUT must be a positive integer"
    exit 1
fi

if [ -z "${VGPU3D_REPEAT_TIMEOUT:-}" ]; then
    repeat_runs=$(( (VGPU3D_XORG_RESTARTS + 1) * VGPU3D_GLXGEARS_RUNS ))
    per_run_budget=$((VGPU3D_STRESS_GLXGEARS_SECONDS + VGPU3D_GLXINFO_TIMEOUT + 2))
    setup_budget=$(( (VGPU3D_XORG_RESTARTS + 1) * 60 ))
    calculated_repeat_timeout=$((repeat_runs * per_run_budget + setup_budget))
    if (( calculated_repeat_timeout < VGPU3D_CMD_TIMEOUT )); then
        calculated_repeat_timeout="${VGPU3D_CMD_TIMEOUT}"
    fi
    export VGPU3D_REPEAT_TIMEOUT="${calculated_repeat_timeout}"
else
    case "${VGPU3D_REPEAT_TIMEOUT}" in
        ''|*[!0-9]*)
            print_error "FAIL: VGPU3D_REPEAT_TIMEOUT must be a positive integer"
            exit 1
            ;;
    esac
    if (( VGPU3D_REPEAT_TIMEOUT < 1 )); then
        print_error "FAIL: VGPU3D_REPEAT_TIMEOUT must be a positive integer"
        exit 1
    fi
    export VGPU3D_REPEAT_TIMEOUT
fi

case "${VGPU3D_PRINT_TIMEOUTS:-0}" in
    1|true|yes)
        printf 'VGPU3D_CMD_TIMEOUT=%s\n' "${VGPU3D_CMD_TIMEOUT}"
        printf 'VGPU3D_STRESS_GLXGEARS_SECONDS=%s\n' "${VGPU3D_STRESS_GLXGEARS_SECONDS}"
        printf 'VGPU3D_REPEAT_TIMEOUT=%s\n' "${VGPU3D_REPEAT_TIMEOUT}"
        exit 0
        ;;
esac

case "${VGPU3D_EXPECT_WINDOW_CLOSE}" in
    1|true|yes)
        if [ -z "${SEMU_TEST_WINDOW_CLOSE_ARM_FILE:-}" ]; then
            print_error "FAIL: window-close stress needs SEMU_TEST_WINDOW_CLOSE_ARM_FILE"
            exit 1
        fi
        case "${VGPU3D_WINDOW_CLOSE_WAIT_TIMEOUT}" in
            ''|*[!0-9]*)
                print_error "FAIL: VGPU3D_WINDOW_CLOSE_WAIT_TIMEOUT must be a positive integer"
                exit 1
                ;;
        esac
        if (( VGPU3D_WINDOW_CLOSE_WAIT_TIMEOUT < 1 )); then
            print_error "FAIL: VGPU3D_WINDOW_CLOSE_WAIT_TIMEOUT must be a positive integer"
            exit 1
        fi
        ;;
esac

case "${HEADLESS}" in
    0|false|no)
        if [ -z "${DISPLAY:-}" ] && [ -z "${WAYLAND_DISPLAY:-}" ]; then
            print_error "FAIL: visible vgpu 3D smoke needs host DISPLAY or WAYLAND_DISPLAY"
            exit 1
        fi
        ;;
esac

if ! command -v sdl2-config >/dev/null 2>&1; then
    print_error "FAIL: vgpu 3D smoke needs sdl2-config in PATH"
    exit 1
fi

case "${ENABLE_VIRGL}" in
    1|true|yes)
        if ! command -v pkg-config >/dev/null 2>&1 ||
           ! pkg-config --exists virglrenderer epoxy gl egl; then
            print_error "FAIL: vgpu 3D smoke needs pkg-config packages: virglrenderer epoxy gl egl"
            exit 1
        fi
        ;;
esac

cleanup
trap cleanup EXIT

echo "Running vgpu 3D smoke: ENABLE_VIRGL=${ENABLE_VIRGL} HEADLESS=${HEADLESS} DISKIMG_FILE=${DISKIMG_FILE} NETDEV=${NETDEV} SMP=${SMP} EXECUTOR=${EXECUTOR} VGPU3D_GLXGEARS_RUNS=${VGPU3D_GLXGEARS_RUNS} VGPU3D_XORG_RESTARTS=${VGPU3D_XORG_RESTARTS} VGPU3D_STRESS_GLXGEARS_SECONDS=${VGPU3D_STRESS_GLXGEARS_SECONDS}"

# Feature toggles are passed through environment variables, which do not
# participate in make's normal dependency tracking. Force a rebuild here so
# virgl/windowed/SMP test runs never reuse a stale semu binary or DTB.
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

# NOTE: We want to capture the 'expect' exit code and map it to our MESSAGES
# array for meaningful error output. Temporarily disable 'errexit' for the
# 'expect' call.
set +e
expect <<'DONE'
set timeout $env(TIMEOUT)
spawn make check DISKIMG_FILE=$env(DISKIMG_FILE)

# Boot and login
expect "buildroot login:" { send "root\r" } timeout { exit 1 }
expect "# "              { send "uname -a\r" } timeout { exit 2 }
expect "riscv32 GNU/Linux" {}

set timeout $env(VGPU3D_CMD_TIMEOUT)

# ---------------- virtio-gpu basic checks ----------------
expect "# " { send "ls -la /dev/dri/ 2>/dev/null || true\r" }
expect "# " { send "if test -c /dev/dri/card0 && test -c /dev/dri/renderD128; then status=OK; else status=MISSING; fi; printf \"__VGPU_DRM_%s__\\n\" \"\$status\"\r" } timeout { exit 3 }
expect {
  -exact "__VGPU_DRM_OK__" {}
  -exact "__VGPU_DRM_MISSING__" { exit 3 }
  timeout { exit 3 }
}

expect "# " {
  send "sh -lc 'if ls /sys/bus/virtio/drivers/virtio_gpu/virtio* >/dev/null 2>&1; then status=OK; else status=BAD; fi; printf \"__VGPU_BIND_%s__\\n\" \"\u0024status\"'\r"
} timeout { exit 3 }
expect {
  -exact "__VGPU_BIND_OK__" {}
  -exact "__VGPU_BIND_BAD__" {
    send "ls -l /sys/bus/virtio/drivers/virtio_gpu/ 2>/dev/null || true\r"
    send "sh -lc 'for d in /sys/bus/virtio/devices/virtio*; do echo \u0024d; ls -l \u0024d/driver 2>/dev/null || true; done'\r"
    exit 3
  }
  timeout { exit 3 }
}

# ---------------- kernel 3D feature evidence ----------------
expect "# " { send "dmesg > /tmp/vgpu3d-dmesg.log; grep -Ei 'virtio.*gpu|drm.*virtio|virgl|capset|resource.*blob|host.*visible' /tmp/vgpu3d-dmesg.log | tail -n 120 || true\r" }

expect "# " { send "if grep -Eiq '\\+virgl' /tmp/vgpu3d-dmesg.log; then status=OK; else status=BAD; fi; printf \"__VGPU_DMESG_VIRGL_%s__\\n\" \"\$status\"\r" } timeout { exit 4 }
expect {
  -exact "__VGPU_DMESG_VIRGL_OK__" {}
  -exact "__VGPU_DMESG_VIRGL_BAD__" { exit 4 }
  timeout { exit 4 }
}

expect "# " { send "if grep -Eiq 'number of cap\[\[:space:]]+sets:\[\[:space:]]*\[1-9]|cap\[\[:space:]]+set.*id\[\[:space:]]+1' /tmp/vgpu3d-dmesg.log; then status=OK; else status=BAD; fi; printf \"__VGPU_DMESG_CAPSET_%s__\\n\" \"\$status\"\r" } timeout { exit 4 }
expect {
  -exact "__VGPU_DMESG_CAPSET_OK__" {}
  -exact "__VGPU_DMESG_CAPSET_BAD__" { exit 4 }
  timeout { exit 4 }
}

expect "# " { send "if grep -Eiq '\\+resource_blob' /tmp/vgpu3d-dmesg.log; then status=OK; else status=BAD; fi; printf \"__VGPU_DMESG_BLOB_%s__\\n\" \"\$status\"\r" } timeout { exit 4 }
expect {
  -exact "__VGPU_DMESG_BLOB_OK__" {}
  -exact "__VGPU_DMESG_BLOB_BAD__" { exit 4 }
  timeout { exit 4 }
}

expect "# " { send "if grep -Eiq '\\+host_visible' /tmp/vgpu3d-dmesg.log; then status=OK; else status=BAD; fi; printf \"__VGPU_DMESG_HOST_VISIBLE_%s__\\n\" \"\$status\"\r" } timeout { exit 4 }
expect {
  -exact "__VGPU_DMESG_HOST_VISIBLE_OK__" {}
  -exact "__VGPU_DMESG_HOST_VISIBLE_BAD__" { exit 4 }
  timeout { exit 4 }
}

# ---------------- Mesa/X11 3D checks from the test-tools disk ----------------
expect "# " { send "if test -f /root/local-env.sh; then status=OK; else status=MISSING; fi; printf \"__LOCALENV_%s__\\n\" \"\$status\"\r" } timeout { exit 5 }
expect {
  -exact "__LOCALENV_OK__" {}
  -exact "__LOCALENV_MISSING__" { exit 5 }
  timeout { exit 5 }
}

expect "# " { send ". /root/local-env.sh >/dev/null 2>&1; if test \$? -eq 0; then status=OK; else status=FAIL; fi; export DISPLAY=:0; printf \"__LOCALENV_SRC_%s__\\n\" \"\$status\"\r" } timeout { exit 5 }
expect {
  -exact "__LOCALENV_SRC_OK__" {}
  -exact "__LOCALENV_SRC_FAIL__" { exit 5 }
  timeout { exit 5 }
}

expect "# " {
  send "ls -l /usr/lib/dri 2>/dev/null || true; if test -f /etc/semu-test-tools-virgl && test -e /usr/lib/dri/virtio_gpu_dri.so; then status=OK; else status=MISSING; fi; printf \"__VIRGL_GUEST_DRIVER_%s__\\n\" \"\$status\"\r"
} timeout { exit 5 }
expect {
  -exact "__VIRGL_GUEST_DRIVER_OK__" {}
  -exact "__VIRGL_GUEST_DRIVER_MISSING__" { exit 5 }
  timeout { exit 5 }
}

expect "# " { send "if command -v glxinfo >/dev/null 2>&1; then status=OK; else status=MISSING; fi; printf \"__GLXINFO_APP_%s__\\n\" \"\$status\"\r" } timeout { exit 5 }
expect {
  -exact "__GLXINFO_APP_OK__" {}
  -exact "__GLXINFO_APP_MISSING__" { exit 5 }
  timeout { exit 5 }
}

expect "# " { send "if command -v glxgears >/dev/null 2>&1; then status=OK; else status=MISSING; fi; printf \"__GLXGEARS_APP_%s__\\n\" \"\$status\"\r" } timeout { exit 5 }
expect {
  -exact "__GLXGEARS_APP_OK__" {}
  -exact "__GLXGEARS_APP_MISSING__" { exit 5 }
  timeout { exit 5 }
}

expect "# " {
  send "if test -S /tmp/.X11-unix/X0; then status=RUNNING; elif command -v Xorg >/dev/null 2>&1; then rm -f /tmp/.X0-lock; Xorg :0 -noreset -nolisten tcp >/tmp/xorg.log 2>&1 & echo \$! >/tmp/xorg.pid; status=STARTED; else status=MISSING; fi; printf \"__VIRGL_XORG_%s__\\n\" \"\$status\"\r"
} timeout { exit 6 }
expect {
  -exact "__VIRGL_XORG_RUNNING__" {}
  -exact "__VIRGL_XORG_STARTED__" {}
  -exact "__VIRGL_XORG_MISSING__" { exit 6 }
  timeout { exit 6 }
}

expect "# " {
  send "i=0; status=FAIL; while test \u0024i -lt $env(VGPU3D_XORG_RETRIES); do if test -S /tmp/.X11-unix/X0; then status=READY; break; fi; sleep $env(VGPU3D_XORG_SLEEP); i=\u0024((i + 1)); done; printf \"__VIRGL_XORG_%s__\\n\" \"\u0024status\"\r"
} timeout { exit 6 }
expect {
  -exact "__VIRGL_XORG_READY__" {}
  -exact "__VIRGL_XORG_FAIL__" {
    send "cat /tmp/xorg.log 2>/dev/null || true\r"
    exit 6
  }
  timeout { exit 6 }
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

expect "# " {
  send_guest_line {cat > /tmp/vgpu3d-repeat.sh <<'VGPU3D_SCRIPT'}
  send_guest_line {#!/bin/sh}
  send_guest_line "xorg_restarts=$env(VGPU3D_XORG_RESTARTS)"
  send_guest_line "xorg_retries=$env(VGPU3D_XORG_RETRIES)"
  send_guest_line "xorg_sleep=$env(VGPU3D_XORG_SLEEP)"
  send_guest_line "glxinfo_retries=$env(VGPU3D_GLXINFO_RETRIES)"
  send_guest_line "glxinfo_timeout=$env(VGPU3D_GLXINFO_TIMEOUT)"
  send_guest_line "glxinfo_sleep=$env(VGPU3D_GLXINFO_SLEEP)"
  send_guest_line "glxgears_runs=$env(VGPU3D_GLXGEARS_RUNS)"
  send_guest_line "glxgears_seconds=$env(VGPU3D_STRESS_GLXGEARS_SECONDS)"
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

restart_xorg() {
  echo "--- restarting Xorg pass $pass ---"
  if test -f /tmp/xorg.pid; then
    kill "$(cat /tmp/xorg.pid)" 2>/dev/null || true
  fi

  if command -v pidof >/dev/null 2>&1; then
    pids=$(pidof Xorg X 2>/dev/null || true)
  else
    pids=$(ps | awk '/[X]org|[X] / {print $1}' 2>/dev/null || true)
  fi
  if test -n "$pids"; then
    kill $pids 2>/dev/null || true
  fi

  sleep 1
  rm -f /tmp/.X0-lock /tmp/.X11-unix/X0 /tmp/xorg.log /tmp/xorg.pid
  Xorg :0 -noreset -nolisten tcp >/tmp/xorg.log 2>&1 &
  echo $! >/tmp/xorg.pid
}

wait_for_xorg() {
  i=0
  ready=FAIL
  while test "$i" -lt "$xorg_retries"; do
    if test -S /tmp/.X11-unix/X0; then
      ready=READY
      break
    fi
    sleep "$xorg_sleep"
    i=$((i + 1))
  done
  test "$ready" = READY
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

pass=0
total=$((xorg_restarts + 1))
while test "$pass" -lt "$total"; do
  if test "$pass" -gt 0; then
    restart_xorg
    if ! wait_for_xorg; then
      print_xorg_logs
      status=XORG
      break
    fi
  fi

  run=1
  while test "$run" -le "$glxgears_runs"; do
    echo "--- glxinfo pass $pass run $run ---"
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
      break
    fi

    echo "--- glxgears pass $pass run $run ---"
    rm -f /tmp/vgpu3d-glxgears.log
    run_glxgears_bounded /tmp/vgpu3d-glxgears.log /tmp/vgpu3d-glxgears-timeout.flag
    rc=$?
    head -40 /tmp/vgpu3d-glxgears.log
    glxgears_status=OK
    if test "$rc" -ne 124; then
      glxgears_status=FAIL
    fi
    if test "$glxgears_status" != OK; then
      print_xorg_logs
      echo '--- dmesg virgl tail ---'
      dmesg | grep -Ei 'virtio.*gpu|drm.*virtio|virgl|capset|resource.*blob|host.*visible' | tail -120 || true
      status=GLXGEARS
      break
    fi

    run=$((run + 1))
  done

  if test "$status" != OK; then
    break
  fi
  pass=$((pass + 1))
done

printf "__VGPU3D_REPEAT_%s__\n" "$status"
}
  send_guest_line {VGPU3D_SCRIPT}
  send_guest_line {chmod +x /tmp/vgpu3d-repeat.sh}
  send_guest_line {/tmp/vgpu3d-repeat.sh}
} timeout { exit 10 } eof { exit 10 }
set timeout $env(VGPU3D_REPEAT_TIMEOUT)
expect {
  -exact "__VGPU3D_REPEAT_OK__" {}
  -exact "__VGPU3D_REPEAT_XORG__" { exit 6 }
  -exact "__VGPU3D_REPEAT_GLXINFO__" { exit 7 }
  -exact "__VGPU3D_REPEAT_GLXGEARS__" { exit 8 }
  eof { exit 10 }
  timeout { exit 10 }
}
set timeout $env(VGPU3D_CMD_TIMEOUT)

set expect_window_close 0
if {[info exists env(VGPU3D_EXPECT_WINDOW_CLOSE)]} {
  set close_value [string tolower $env(VGPU3D_EXPECT_WINDOW_CLOSE)]
  if {$close_value eq "1" || $close_value eq "true" || $close_value eq "yes"} {
    set expect_window_close 1
  }
}

if {$expect_window_close} {
  if {![info exists env(SEMU_TEST_WINDOW_CLOSE_ARM_FILE)] ||
      $env(SEMU_TEST_WINDOW_CLOSE_ARM_FILE) eq ""} {
    exit 9
  }

  set arm_file $env(SEMU_TEST_WINDOW_CLOSE_ARM_FILE)
  if {[catch {set fh [open $arm_file w]} err]} {
    puts stderr "failed to arm host window-close hook: $err"
    exit 9
  }
  close $fh
  puts "__VGPU_WINDOW_CLOSE_ARMED__"

  set timeout $env(VGPU3D_WINDOW_CLOSE_WAIT_TIMEOUT)
  expect eof {
    set wait_status [wait]
    set close_rc [lindex $wait_status 3]
    exit $close_rc
  } timeout {
    exit 9
  }
}
DONE

ret="$?"
set -e  # Re-enable 'errexit' after capturing 'expect' return code.

MESSAGES=(
  "PASS: windowed virtio-gpu virgl 3D checks"
  "FAIL: boot/login prompt not found"
  "FAIL: shell prompt not found"
  "FAIL: virtio-gpu basic checks failed (/dev/dri/card0, /dev/dri/renderD128, or virtio_gpu binding)"
  "FAIL: virgl/capset/resource_blob/host_visible dmesg checks failed"
  "FAIL: guest VirGL tools/driver missing or local-env.sh failed"
  "FAIL: guest Xorg did not start on :0"
  "FAIL: glxinfo -B failed or did not report a virgl renderer"
  "FAIL: glxgears did not start cleanly"
  "FAIL: host window-close trigger did not stop semu"
  "FAIL: vgpu 3D repeat script timed out waiting for completion sentinel"
)

if [[ "${ret}" -eq 0 ]]; then
  print_success "${MESSAGES[0]}"
  exit 0
fi

print_error "${MESSAGES[${ret}]:-FAIL: unknown error (exit code ${ret})}"
exit "${ret}"
