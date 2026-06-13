#include "virtio-irq.h"

#include <stddef.h>

static bool virtio_irq_source_valid(enum semu_irq_source source)
{
    return semu_irq_source_plic_bit(source) != 0;
}

int virtio_irq_init(struct virtio_irq *irq,
                    emu_state_t *emu,
                    enum semu_irq_source source)
{
    if (!irq || !emu || !virtio_irq_source_valid(source))
        return -1;

    atomic_init(&irq->status, 0);
    irq->source = source;
    irq->emu = emu;
    irq->initialized = false;

    if (pthread_mutex_init(&irq->lock, NULL) != 0)
        return -1;

    irq->initialized = true;
    return 0;
}

void virtio_irq_destroy(struct virtio_irq *irq)
{
    if (!irq || !irq->initialized)
        return;

    pthread_mutex_lock(&irq->lock);
    atomic_store_explicit(&irq->status, 0, memory_order_release);
    semu_irq_source_set(irq->emu, irq->source, false);
    irq->initialized = false;
    pthread_mutex_unlock(&irq->lock);
    pthread_mutex_destroy(&irq->lock);
}

void virtio_irq_trigger(struct virtio_irq *irq, uint32_t bits)
{
    uint32_t old_status;

    if (!irq || !irq->initialized || bits == 0)
        return;

    pthread_mutex_lock(&irq->lock);
    old_status =
        atomic_fetch_or_explicit(&irq->status, bits, memory_order_release);
    if (old_status == 0)
        semu_irq_source_set(irq->emu, irq->source, true);
    pthread_mutex_unlock(&irq->lock);
}

void virtio_irq_ack(struct virtio_irq *irq, uint32_t bits)
{
    uint32_t old_status;
    uint32_t new_status;

    if (!irq || !irq->initialized || bits == 0)
        return;

    pthread_mutex_lock(&irq->lock);
    old_status =
        atomic_fetch_and_explicit(&irq->status, ~bits, memory_order_acq_rel);
    new_status = old_status & ~bits;
    if (old_status != 0 && new_status == 0)
        semu_irq_source_set(irq->emu, irq->source, false);
    pthread_mutex_unlock(&irq->lock);
}

uint32_t virtio_irq_read_status(struct virtio_irq *irq)
{
    if (!irq || !irq->initialized)
        return 0;

    return atomic_load_explicit(&irq->status, memory_order_acquire);
}
