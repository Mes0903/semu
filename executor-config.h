#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hart-executor.h"

enum semu_executor_mode {
    SEMU_EXECUTOR_SINGLE_THREAD,
    SEMU_EXECUTOR_THREADED_CPU_WITH_LEGACY_DEVICE_GATE,
    SEMU_EXECUTOR_THREADED_CPU_WITH_DEVICE_ACTORS,
};

#define SEMU_EXECUTOR_DEVICE_GATE_UNSUPPORTED_CAP 160

struct semu_executor_virtio_host_io_policy {
    const char *device_name;
    bool build_enabled;
    bool actor_mode_allowed;
    const char *actor_mode_policy;
};

struct semu_executor_device_gate {
    bool allowed;
    char unsupported_devices[SEMU_EXECUTOR_DEVICE_GATE_UNSUPPORTED_CAP];
    const char *fallback_command;
};

int semu_executor_mode_parse(const char *name, enum semu_executor_mode *mode);
const char *semu_executor_mode_name(enum semu_executor_mode mode);
enum semu_executor_mode semu_executor_default_mode(uint32_t hart_count);
enum hart_executor_backend semu_executor_backend_for_mode(
    enum semu_executor_mode mode);
size_t semu_executor_virtio_host_io_policy_count(void);
const struct semu_executor_virtio_host_io_policy *
semu_executor_virtio_host_io_policy_at(size_t index);
struct semu_executor_device_gate semu_executor_check_actor_device_gate(
    enum semu_executor_mode mode);
