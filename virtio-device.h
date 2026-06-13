#pragma once

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "irq-source.h"
#include "ram_access.h"
#include "virtio-irq.h"
#include "virtq.h"

struct virtio_device_common;

struct virtio_activation_context {
    emu_state_t *emu;
    struct virtio_device_common *common;
    struct virtq *queues;
    uint16_t num_queues;
    struct virtio_irq *irq;
    uint64_t generation;
};

struct virtio_device_ops {
    int (*activate)(void *opaque, const struct virtio_activation_context *ctx);
    int (*prepare_reset)(void *opaque,
                         uint64_t old_generation,
                         uint64_t new_generation);
    int (*reset)(void *opaque,
                 uint64_t old_generation,
                 uint64_t new_generation);
    int (*notify_queue)(void *opaque,
                        uint16_t queue_index,
                        uint64_t generation);
    uint32_t (*read_config)(void *opaque, uint32_t offset, uint32_t size);
    void (*write_config)(void *opaque,
                         uint32_t offset,
                         uint32_t size,
                         uint32_t value);
};

struct virtio_queue_common {
    uint16_t max_size;
    uint16_t queue_num;
    guest_paddr_t desc_addr;
    guest_paddr_t driver_addr;
    guest_paddr_t device_addr;
};

struct virtio_device_common {
    uint32_t device_id;
    uint32_t vendor_id;
    uint64_t device_features;
    uint64_t driver_features;
    uint32_t device_features_sel;
    uint32_t driver_features_sel;
    uint32_t queue_sel;
    struct virtq *queues;
    uint16_t num_queues;
    atomic_uint status;
    struct virtio_irq irq;
    uint32_t config_generation;
    uint64_t generation;
    pthread_mutex_t transport_lock;
    pthread_mutex_t backend_lock;

    emu_state_t *emu;
    ram_dma_t *dma;
    uint64_t required_features;
    const struct virtio_device_ops *ops;
    void *opaque;
    struct virtio_queue_common *queue_cfgs;
    bool initialized;
    bool irq_initialized;
    bool activated;
    bool reset_in_progress;
};

struct virtio_device_common_config {
    emu_state_t *emu;
    ram_dma_t *dma;
    enum semu_irq_source irq_source;
    uint32_t device_id;
    uint32_t vendor_id;
    uint64_t device_features;
    uint64_t required_features;
    const uint16_t *queue_max_sizes;
    uint16_t num_queues;
    const struct virtio_device_ops *ops;
    void *opaque;
};

int virtio_device_common_init(struct virtio_device_common *common,
                              const struct virtio_device_common_config *config);
void virtio_device_common_destroy(struct virtio_device_common *common);
int virtio_device_common_reset(struct virtio_device_common *common);
void virtio_device_common_set_needs_reset(struct virtio_device_common *common);
