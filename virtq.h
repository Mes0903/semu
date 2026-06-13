#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "guest-types.h"
#include "ram_access.h"

#define VRING_AVAIL_F_NO_INTERRUPT 1
#define VIRTQ_F_INDIRECT_DESC (UINT64_C(1) << 28)

struct virtq_iov {
    guest_paddr_t addr;
    uint32_t len;
};

struct virtq_chain {
    uint16_t head;
    struct virtq_iov *readable;
    size_t readable_capacity;
    size_t readable_count;
    struct virtq_iov *writable;
    size_t writable_capacity;
    size_t writable_count;
};

struct virtq {
    uint16_t queue_size;
    uint16_t last_avail;
    bool ready;
    guest_paddr_t desc_addr;
    guest_paddr_t driver_addr;
    guest_paddr_t device_addr;
    uint16_t used_idx;
    uint64_t features;
};

void virtq_init(struct virtq *vq);
int virtq_configure(struct virtq *vq,
                    const ram_dma_t *dma,
                    uint16_t queue_size,
                    guest_paddr_t desc_addr,
                    guest_paddr_t driver_addr,
                    guest_paddr_t device_addr,
                    uint64_t features);
int virtq_validate(const struct virtq *vq, const ram_dma_t *dma);
int virtq_pop(const ram_dma_t *dma,
              struct virtq *vq,
              struct virtq_chain *chain);
int virtq_add_used(ram_dma_t *dma, struct virtq *vq, uint32_t id, uint32_t len);
bool virtq_interrupt_suppressed(const ram_dma_t *dma, const struct virtq *vq);
