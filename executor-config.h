#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "hart-executor.h"

enum semu_executor_mode {
    SEMU_EXECUTOR_SINGLE_THREAD,
    SEMU_EXECUTOR_THREADED_CPU_WITH_LEGACY_DEVICE_GATE,
    SEMU_EXECUTOR_THREADED_CPU_WITH_DEVICE_ACTORS,
};

struct semu_executor_device_gate {
    bool allowed;
    const char *unsupported_devices;
    const char *fallback_command;
};

int semu_executor_mode_parse(const char *name, enum semu_executor_mode *mode);
const char *semu_executor_mode_name(enum semu_executor_mode mode);
enum semu_executor_mode semu_executor_default_mode(uint32_t hart_count);
enum hart_executor_backend semu_executor_backend_for_mode(
    enum semu_executor_mode mode);
struct semu_executor_device_gate semu_executor_check_actor_device_gate(
    enum semu_executor_mode mode);
