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

static const struct semu_executor_virtio_host_io_policy
    virtio_host_io_policies[] = {
        {
            .device_name = "virtio-gpu",
            .build_enabled = SEMU_HAS(VIRTIOGPU),
            .actor_mode_allowed = true,
            .actor_mode_policy = "first actor client; QueueNotify must wake "
                                 "actor, not parse on vCPU",
        },
        {
            .device_name = "virtio-input",
            .build_enabled = SEMU_HAS(VIRTIOINPUT),
            .actor_mode_allowed = false,
            .actor_mode_policy =
                "keep SDL producer/SPSC queue; move guest virtqueues to common "
                "transport before actor support",
        },
        {
            .device_name = "virtio-rng",
            .build_enabled = SEMU_HAS(VIRTIORNG),
            .actor_mode_allowed = true,
            .actor_mode_policy = "actor-backed; QueueNotify wakes actor, "
                                 "entropy read runs outside vCPU",
        },
        {
            .device_name = "virtio-blk",
            .build_enabled = SEMU_HAS(VIRTIOBLK),
            .actor_mode_allowed = false,
            .actor_mode_policy =
                "actor or actor+worker pool required; flush/drain runs outside "
                "lifecycle/transport locks",
        },
        {
            .device_name = "virtio-fs",
            .build_enabled = SEMU_HAS(VIRTIOFS),
            .actor_mode_allowed = false,
            .actor_mode_policy =
                "actor or actor+worker pool required; host filesystem calls "
                "need cancellation or generation bounds",
        },
        {
            .device_name = "virtio-net",
            .build_enabled = SEMU_HAS(VIRTIONET),
            .actor_mode_allowed = false,
            .actor_mode_policy = "host fd/slirp/vmnet events need device event "
                                 "loop or actor wake path; no tick polling",
        },
        {
            .device_name = "virtio-snd",
            .build_enabled = SEMU_HAS(VIRTIOSND),
            .actor_mode_allowed = false,
            .actor_mode_policy =
                "actor owns virtqueues; callback owns timing; reset releases, "
                "broadcasts, closes, then frees buffers",
        },
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

size_t semu_executor_virtio_host_io_policy_count(void)
{
    return ARRAY_SIZE(virtio_host_io_policies);
}

const struct semu_executor_virtio_host_io_policy *
semu_executor_virtio_host_io_policy_at(size_t index)
{
    if (index >= ARRAY_SIZE(virtio_host_io_policies))
        return NULL;
    return &virtio_host_io_policies[index];
}

static void append_unsupported_device(char *buffer,
                                      size_t buffer_size,
                                      const char *device_name)
{
    size_t len;

    if (buffer_size == 0)
        return;

    len = strlen(buffer);
    if (len >= buffer_size - 1)
        return;

    if (len > 0) {
        strncat(buffer, ", ", buffer_size - len - 1);
        len = strlen(buffer);
        if (len >= buffer_size - 1)
            return;
    }

    strncat(buffer, device_name, buffer_size - len - 1);
}

static bool build_actor_unsupported_devices(char *buffer, size_t buffer_size)
{
    bool has_unsupported = false;

    if (buffer_size > 0)
        buffer[0] = '\0';

    for (size_t i = 0; i < ARRAY_SIZE(virtio_host_io_policies); i++) {
        const struct semu_executor_virtio_host_io_policy *policy =
            &virtio_host_io_policies[i];

        if (!policy->build_enabled || policy->actor_mode_allowed)
            continue;

        append_unsupported_device(buffer, buffer_size, policy->device_name);
        has_unsupported = true;
    }

    return has_unsupported;
}

struct semu_executor_device_gate semu_executor_check_actor_device_gate(
    enum semu_executor_mode mode)
{
    struct semu_executor_device_gate gate = {
        .allowed = true,
        .unsupported_devices = "",
        .fallback_command = SEMU_EXECUTOR_FALLBACK,
    };

    if (mode != SEMU_EXECUTOR_THREADED_CPU_WITH_DEVICE_ACTORS)
        return gate;

    gate.allowed = !build_actor_unsupported_devices(
        gate.unsupported_devices, sizeof(gate.unsupported_devices));
    return gate;
}
