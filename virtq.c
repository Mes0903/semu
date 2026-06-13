#include "virtq.h"

#include "virtio.h"

#include <errno.h>
#include <string.h>

static bool virtq_range_ok(const ram_dma_t *dma,
                           guest_paddr_t addr,
                           guest_size_t len)
{
    if (!dma || !dma->words)
        return false;
    if (len == 0)
        return addr <= dma->byte_size;
    if (addr >= dma->byte_size)
        return false;
    return len <= dma->byte_size - addr;
}

static bool virtq_range_len_ok(const ram_dma_t *dma,
                               guest_paddr_t addr,
                               guest_size_t count,
                               guest_size_t elem_size,
                               guest_size_t fixed_size)
{
    guest_size_t bytes;

    if (count > (UINT64_MAX - fixed_size) / elem_size)
        return false;
    bytes = fixed_size + count * elem_size;
    return virtq_range_ok(dma, addr, bytes);
}

static int virtq_validate_layout(const ram_dma_t *dma,
                                 uint16_t queue_size,
                                 guest_paddr_t desc_addr,
                                 guest_paddr_t driver_addr,
                                 guest_paddr_t device_addr)
{
    if (!dma || queue_size == 0)
        return -EINVAL;
    if (!virtq_range_len_ok(dma, desc_addr, queue_size,
                            sizeof(struct virtq_desc), 0))
        return -EFAULT;
    if (!virtq_range_len_ok(dma, driver_addr, queue_size, sizeof(uint16_t), 4))
        return -EFAULT;
    if (!virtq_range_len_ok(dma, device_addr, queue_size, 8, 4))
        return -EFAULT;
    return 0;
}

static bool virtq_read_u16(const ram_dma_t *dma,
                           guest_paddr_t addr,
                           uint16_t *value)
{
    return ram_dma_read(dma, addr, value, sizeof(*value));
}

static void virtq_chain_clear_counts(struct virtq_chain *chain)
{
    if (!chain)
        return;

    chain->head = 0;
    chain->readable_count = 0;
    chain->writable_count = 0;
}

static int virtq_append_iov(struct virtq_chain *chain,
                            const struct virtq_desc *desc,
                            size_t *readable_count,
                            size_t *writable_count)
{
    struct virtq_iov *iov;
    size_t *count;
    size_t capacity;

    if (desc->flags & VIRTIO_DESC_F_WRITE) {
        iov = chain->writable;
        count = writable_count;
        capacity = chain->writable_capacity;
    } else {
        iov = chain->readable;
        count = readable_count;
        capacity = chain->readable_capacity;
    }

    if (*count >= capacity || !iov)
        return -ENOSPC;

    iov[*count].addr = desc->addr;
    iov[*count].len = desc->len;
    (*count)++;
    return 0;
}

static int virtq_walk_desc_table(const ram_dma_t *dma,
                                 const struct virtq *vq,
                                 guest_paddr_t table_addr,
                                 uint16_t table_size,
                                 uint16_t head,
                                 bool indirect_table,
                                 struct virtq_chain *chain,
                                 size_t *readable_count,
                                 size_t *writable_count)
{
    uint16_t desc_index = head;

    if (table_size == 0 || head >= table_size)
        return -EINVAL;

    for (size_t seen = 0;; seen++) {
        struct virtq_desc desc;
        int ret;

        if (seen >= table_size)
            return -ELOOP;
        if (!ram_dma_read(
                dma, table_addr + (guest_paddr_t) desc_index * sizeof(desc),
                &desc, sizeof(desc)))
            return -EFAULT;

        if (desc.flags & VIRTIO_DESC_F_INDIRECT) {
            uint16_t indirect_count;

            if (indirect_table)
                return -ENOTSUP;
            if (!(vq->features & VIRTQ_F_INDIRECT_DESC))
                return -ENOTSUP;
            if (desc.flags & (VIRTIO_DESC_F_NEXT | VIRTIO_DESC_F_WRITE))
                return -EINVAL;
            if (desc.len == 0 || desc.len % sizeof(struct virtq_desc) != 0)
                return -EINVAL;
            if (desc.len / sizeof(struct virtq_desc) > vq->queue_size)
                return -EINVAL;
            if (!virtq_range_ok(dma, desc.addr, desc.len))
                return -EFAULT;

            indirect_count = (uint16_t) (desc.len / sizeof(struct virtq_desc));
            return virtq_walk_desc_table(dma, vq, desc.addr, indirect_count, 0,
                                         true, chain, readable_count,
                                         writable_count);
        }

        if (!virtq_range_ok(dma, desc.addr, desc.len))
            return -EFAULT;

        ret = virtq_append_iov(chain, &desc, readable_count, writable_count);
        if (ret < 0)
            return ret;

        if (!(desc.flags & VIRTIO_DESC_F_NEXT))
            return 0;
        if (desc.next >= table_size)
            return -EINVAL;
        desc_index = desc.next;
    }
}

void virtq_init(struct virtq *vq)
{
    if (!vq)
        return;
    memset(vq, 0, sizeof(*vq));
}

int virtq_configure(struct virtq *vq,
                    const ram_dma_t *dma,
                    uint16_t queue_size,
                    guest_paddr_t desc_addr,
                    guest_paddr_t driver_addr,
                    guest_paddr_t device_addr,
                    uint64_t features)
{
    uint16_t avail_idx;
    uint16_t used_idx;
    int ret;

    if (!vq)
        return -EINVAL;

    ret = virtq_validate_layout(dma, queue_size, desc_addr, driver_addr,
                                device_addr);
    if (ret < 0) {
        vq->ready = false;
        return ret;
    }
    if (!virtq_read_u16(dma, driver_addr + 2, &avail_idx) ||
        !virtq_read_u16(dma, device_addr + 2, &used_idx)) {
        vq->ready = false;
        return -EFAULT;
    }

    vq->queue_size = queue_size;
    vq->last_avail = avail_idx;
    vq->ready = true;
    vq->desc_addr = desc_addr;
    vq->driver_addr = driver_addr;
    vq->device_addr = device_addr;
    vq->used_idx = used_idx;
    vq->features = features;
    return 0;
}

int virtq_validate(const struct virtq *vq, const ram_dma_t *dma)
{
    if (!vq)
        return -EINVAL;
    if (!vq->ready)
        return -EINVAL;
    return virtq_validate_layout(dma, vq->queue_size, vq->desc_addr,
                                 vq->driver_addr, vq->device_addr);
}

int virtq_pop(const ram_dma_t *dma, struct virtq *vq, struct virtq_chain *chain)
{
    uint16_t avail_idx;
    uint16_t head;
    size_t readable_count = 0;
    size_t writable_count = 0;
    int ret;

    if (!vq || !chain)
        return -EINVAL;

    virtq_chain_clear_counts(chain);
    ret = virtq_validate(vq, dma);
    if (ret < 0)
        return ret;
    if (!virtq_read_u16(dma, vq->driver_addr + 2, &avail_idx))
        return -EFAULT;
    if (avail_idx == vq->last_avail)
        return 0;

    __atomic_thread_fence(__ATOMIC_ACQUIRE);

    if (!virtq_read_u16(dma,
                        vq->driver_addr + 4 +
                            (guest_paddr_t) (vq->last_avail % vq->queue_size) *
                                sizeof(uint16_t),
                        &head))
        return -EFAULT;
    if (head >= vq->queue_size)
        return -EINVAL;

    ret = virtq_walk_desc_table(dma, vq, vq->desc_addr, vq->queue_size, head,
                                false, chain, &readable_count, &writable_count);
    if (ret < 0) {
        virtq_chain_clear_counts(chain);
        return ret;
    }

    chain->head = head;
    chain->readable_count = readable_count;
    chain->writable_count = writable_count;
    vq->last_avail++;
    return 1;
}

int virtq_add_used(ram_dma_t *dma, struct virtq *vq, uint32_t id, uint32_t len)
{
    guest_paddr_t elem_addr;
    uint16_t next_used;
    int ret;

    if (!vq)
        return -EINVAL;
    if (id >= vq->queue_size)
        return -EINVAL;

    ret = virtq_validate(vq, dma);
    if (ret < 0)
        return ret;

    elem_addr = vq->device_addr + 4 +
                (guest_paddr_t) (vq->used_idx % vq->queue_size) * 8;
    if (!ram_dma_write(dma, elem_addr, &id, sizeof(id)) ||
        !ram_dma_write(dma, elem_addr + 4, &len, sizeof(len)))
        return -EFAULT;

    next_used = vq->used_idx + 1;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    if (!ram_dma_write(dma, vq->device_addr + 2, &next_used, sizeof(next_used)))
        return -EFAULT;

    vq->used_idx = next_used;
    return 0;
}

bool virtq_interrupt_suppressed(const ram_dma_t *dma, const struct virtq *vq)
{
    uint16_t flags;

    if (virtq_validate(vq, dma) < 0)
        return false;
    if (!virtq_read_u16(dma, vq->driver_addr, &flags))
        return false;
    return (flags & VRING_AVAIL_F_NO_INTERRUPT) != 0;
}
