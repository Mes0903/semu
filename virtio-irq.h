#pragma once

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "irq-source.h"

struct virtio_irq {
    atomic_uint status;
    enum semu_irq_source source;
    emu_state_t *emu;
    pthread_mutex_t lock;
    bool initialized;
};

int virtio_irq_init(struct virtio_irq *irq,
                    emu_state_t *emu,
                    enum semu_irq_source source);
void virtio_irq_destroy(struct virtio_irq *irq);
void virtio_irq_trigger(struct virtio_irq *irq, uint32_t bits);
void virtio_irq_ack(struct virtio_irq *irq, uint32_t bits);
uint32_t virtio_irq_read_status(struct virtio_irq *irq);
