#include "executor-config.h"

#include <string.h>

#include "common.h"

#define SEMU_EXECUTOR_FALLBACK "--executor=threaded-cpu-with-legacy-device-gate"

struct executor_mode_entry {
    const char *name;
    enum semu_executor_mode mode;
};

static const struct executor_mode_entry executor_modes[] = {
    {"single-thread", SEMU_EXECUTOR_SINGLE_THREAD},
    {"threaded-cpu-with-legacy-device-gate",
     SEMU_EXECUTOR_THREADED_CPU_WITH_LEGACY_DEVICE_GATE},
    {"threaded-cpu-with-device-actors",
     SEMU_EXECUTOR_THREADED_CPU_WITH_DEVICE_ACTORS},
};

int semu_executor_mode_parse(const char *name, enum semu_executor_mode *mode)
{
    if (!name || !mode)
        return -1;

    for (size_t i = 0; i < ARRAY_SIZE(executor_modes); i++) {
        if (strcmp(name, executor_modes[i].name) == 0) {
            *mode = executor_modes[i].mode;
            return 0;
        }
    }
    return -1;
}

const char *semu_executor_mode_name(enum semu_executor_mode mode)
{
    for (size_t i = 0; i < ARRAY_SIZE(executor_modes); i++) {
        if (executor_modes[i].mode == mode)
            return executor_modes[i].name;
    }
    return "unknown";
}

enum semu_executor_mode semu_executor_default_mode(uint32_t hart_count)
{
    if (hart_count <= 1)
        return SEMU_EXECUTOR_SINGLE_THREAD;
    return SEMU_EXECUTOR_THREADED_CPU_WITH_LEGACY_DEVICE_GATE;
}

enum hart_executor_backend semu_executor_backend_for_mode(
    enum semu_executor_mode mode)
{
    switch (mode) {
    case SEMU_EXECUTOR_SINGLE_THREAD:
        return HART_EXEC_SINGLE_THREAD;
    case SEMU_EXECUTOR_THREADED_CPU_WITH_LEGACY_DEVICE_GATE:
    case SEMU_EXECUTOR_THREADED_CPU_WITH_DEVICE_ACTORS:
        return HART_EXEC_DEDICATED_THREADS;
    default:
        return HART_EXEC_DEDICATED_THREADS;
    }
}

struct semu_executor_device_gate semu_executor_check_actor_device_gate(
    enum semu_executor_mode mode)
{
    if (mode != SEMU_EXECUTOR_THREADED_CPU_WITH_DEVICE_ACTORS) {
        return (struct semu_executor_device_gate) {
            .allowed = true,
            .unsupported_devices = "",
            .fallback_command = SEMU_EXECUTOR_FALLBACK,
        };
    }

#if SEMU_HAS(VIRTIOINPUT) || SEMU_HAS(VIRTIONET) || SEMU_HAS(VIRTIOBLK) || \
    SEMU_HAS(VIRTIORNG) || SEMU_HAS(VIRTIOSND) || SEMU_HAS(VIRTIOFS)
    static const char unsupported_devices[] =
#if SEMU_HAS(VIRTIOINPUT)
        "virtio-input"
#endif
#if SEMU_HAS(VIRTIOINPUT) &&                                              \
    (SEMU_HAS(VIRTIONET) || SEMU_HAS(VIRTIOBLK) || SEMU_HAS(VIRTIORNG) || \
     SEMU_HAS(VIRTIOSND) || SEMU_HAS(VIRTIOFS))
        ", "
#endif
#if SEMU_HAS(VIRTIONET)
        "virtio-net"
#endif
#if SEMU_HAS(VIRTIONET) && (SEMU_HAS(VIRTIOBLK) || SEMU_HAS(VIRTIORNG) || \
                            SEMU_HAS(VIRTIOSND) || SEMU_HAS(VIRTIOFS))
        ", "
#endif
#if SEMU_HAS(VIRTIOBLK)
        "virtio-blk"
#endif
#if SEMU_HAS(VIRTIOBLK) && \
    (SEMU_HAS(VIRTIORNG) || SEMU_HAS(VIRTIOSND) || SEMU_HAS(VIRTIOFS))
        ", "
#endif
#if SEMU_HAS(VIRTIORNG)
        "virtio-rng"
#endif
#if SEMU_HAS(VIRTIORNG) && (SEMU_HAS(VIRTIOSND) || SEMU_HAS(VIRTIOFS))
        ", "
#endif
#if SEMU_HAS(VIRTIOSND)
        "virtio-snd"
#endif
#if SEMU_HAS(VIRTIOSND) && SEMU_HAS(VIRTIOFS)
        ", "
#endif
#if SEMU_HAS(VIRTIOFS)
        "virtio-fs"
#endif
        ;
    return (struct semu_executor_device_gate) {
        .allowed = false,
        .unsupported_devices = unsupported_devices,
        .fallback_command = SEMU_EXECUTOR_FALLBACK,
    };
#else
    return (struct semu_executor_device_gate) {
        .allowed = true,
        .unsupported_devices = "",
        .fallback_command = SEMU_EXECUTOR_FALLBACK,
    };
#endif
}
