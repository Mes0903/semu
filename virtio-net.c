#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/poll.h>
#include <sys/uio.h>
#include <unistd.h>

#include "common.h"
#include "device.h"
#include "ram_access.h"
#include "riscv.h"
#include "riscv_private.h"
#include "virtio-irq.h"
#include "virtio-mmio.h"
#include "virtio.h"
#include "virtq.h"

#define VIRTIO_NET_F_MRG_RXBUF (UINT64_C(1) << 15)
#define VIRTIO_NET_F_VERSION_1 (UINT64_C(1) << 32)

#define VNET_QUEUE_NUM_MAX 1024
#define VNET_LEGACY_HEADER_LEN 10
#define VNET_V1_HEADER_LEN 12
#define VNET_PACKET_MAX SLIRP_PKT_MAX

enum { VNET_QUEUE_RX = 0, VNET_QUEUE_TX = 1, VNET_QUEUE_COUNT = 2 };

enum {
    VNET_EVENT_BACKEND = 0,
    VNET_EVENT_USER_RX = 1,
    VNET_EVENT_USER_SLIRP_IN = 2,
    VNET_EVENT_USER_TX = 3,
    VNET_EVENT_USER_DYNAMIC = 4,
    VNET_EVENT_COUNT = 5,
};

PACKED(struct virtio_net_config {
    uint8_t mac[6];
    uint16_t status;
    uint16_t max_virtqueue_pairs;
    uint16_t mtu;
});

struct virtio_net_priv {
    struct virtio_net_config config;
    bool peer_owned;
    atomic_uint header_len;
};

#define PRIV(vnet) (&((struct virtio_net_priv *) (vnet)->priv)->config)
#define VNET_PRIV(vnet) ((struct virtio_net_priv *) (vnet)->priv)

static inline unsigned virtio_net_status_load(virtio_net_state_t *vnet)
{
    return atomic_load_explicit(&vnet->common.status, memory_order_acquire);
}

static unsigned virtio_net_features_header_len(uint64_t features)
{
    if (features & (VIRTIO_NET_F_VERSION_1 | VIRTIO_NET_F_MRG_RXBUF))
        return VNET_V1_HEADER_LEN;
    return VNET_LEGACY_HEADER_LEN;
}

static unsigned virtio_net_header_len(virtio_net_state_t *vnet)
{
    struct virtio_net_priv *priv = vnet ? VNET_PRIV(vnet) : NULL;
    unsigned header_len;

    if (!priv)
        return VNET_LEGACY_HEADER_LEN;

    header_len = atomic_load_explicit(&priv->header_len, memory_order_acquire);
    return header_len ? header_len : VNET_LEGACY_HEADER_LEN;
}

static void virtio_net_set_queue_fd_ready(virtio_net_state_t *vnet,
                                          uint16_t queue_index,
                                          bool ready)
{
    if (!vnet || queue_index >= VNET_QUEUE_COUNT)
        return;

    atomic_store_explicit(&vnet->queues[queue_index].fd_ready, ready,
                          memory_order_release);
}

static bool virtio_net_queue_fd_ready(virtio_net_state_t *vnet,
                                      uint16_t queue_index)
{
    if (!vnet || queue_index >= VNET_QUEUE_COUNT)
        return false;

    return atomic_load_explicit(&vnet->queues[queue_index].fd_ready,
                                memory_order_acquire);
}

static void virtio_net_set_fail(virtio_net_state_t *vnet)
{
    unsigned status = virtio_net_status_load(vnet);

    virtio_device_common_set_needs_reset(&vnet->common);
    if (status & VIRTIO_STATUS__DRIVER_OK)
        virtio_irq_trigger(&vnet->common.irq, VIRTIO_INT__CONF_CHANGE);
}

static bool virtio_net_config_range_valid(uint32_t offset, uint32_t size)
{
    return size != 0 && offset < sizeof(struct virtio_net_config) &&
           size <= sizeof(struct virtio_net_config) - offset;
}

static uint32_t virtio_net_read_config(void *opaque,
                                       uint32_t offset,
                                       uint32_t size)
{
    virtio_net_state_t *vnet = opaque;
    uint32_t value = 0;

    if (!vnet || !vnet->priv || !virtio_net_config_range_valid(offset, size))
        return 0;

    memcpy(&value, (uint8_t *) PRIV(vnet) + offset, size);
    return value;
}

static void virtio_net_write_config(void *opaque,
                                    uint32_t offset,
                                    uint32_t size,
                                    uint32_t value)
{
    virtio_net_state_t *vnet = opaque;

    if (!vnet || !vnet->priv || !virtio_net_config_range_valid(offset, size))
        return;

    memcpy((uint8_t *) PRIV(vnet) + offset, &value, size);
}

static bool virtio_net_queue_ready_for_actor(virtio_net_state_t *vnet,
                                             const struct virtq *queue,
                                             uint16_t queue_index)
{
    unsigned status;

    if (!vnet || !queue || !queue->ready)
        return false;
    if (!vnet->peer.op)
        return false;
    if (!virtio_net_queue_fd_ready(vnet, queue_index))
        return false;

    status = virtio_net_status_load(vnet);
    return !(status & VIRTIO_STATUS__DEVICE_NEEDS_RESET) &&
           (status & VIRTIO_STATUS__DRIVER_OK);
}

static int virtio_net_queue_available(virtio_net_state_t *vnet,
                                      struct virtq *queue,
                                      uint16_t *available)
{
    uint16_t avail_idx;
    uint16_t delta;

    if (!vnet || !queue || !queue->ready || !available)
        return -EINVAL;

    if (!ram_dma_read(vnet->common.dma, queue->driver_addr + 2, &avail_idx,
                      sizeof(avail_idx)))
        return -EFAULT;

    delta = (uint16_t) (avail_idx - queue->last_avail);
    if (delta > queue->queue_size)
        return -EINVAL;

    *available = delta;
    return 0;
}

static bool virtio_net_actor_generation_current(virtio_net_state_t *vnet,
                                                struct virtio_actor *actor,
                                                uint64_t generation)
{
    (void) vnet;
    return actor && virtio_actor_generation(actor) == generation;
}

static bool virtio_net_common_generation_current(virtio_net_state_t *vnet,
                                                 uint64_t generation)
{
    bool current;

    if (!vnet || !vnet->common.initialized)
        return false;

    pthread_mutex_lock(&vnet->common.transport_lock);
    current = vnet->common.generation == generation &&
              !vnet->common.reset_in_progress;
    pthread_mutex_unlock(&vnet->common.transport_lock);
    return current;
}

static bool virtio_net_capture_common_generation(virtio_net_state_t *vnet,
                                                 uint64_t *generation)
{
    unsigned status;
    bool current;

    if (!vnet || !vnet->common.initialized || !generation)
        return false;

    pthread_mutex_lock(&vnet->common.transport_lock);
    status = virtio_net_status_load(vnet);
    current = !vnet->common.reset_in_progress &&
              (status & VIRTIO_STATUS__DRIVER_OK) &&
              !(status & VIRTIO_STATUS__DEVICE_NEEDS_RESET);
    if (current)
        *generation = vnet->common.generation;
    pthread_mutex_unlock(&vnet->common.transport_lock);
    return current;
}

static bool virtio_net_begin_actor_completion(virtio_net_state_t *vnet,
                                              uint64_t actor_generation,
                                              uint64_t common_generation)
{
    bool common_current;

    if (!vnet || !vnet->actor_initialized || !vnet->common.initialized)
        return false;

    pthread_mutex_lock(&vnet->common.transport_lock);
    common_current = vnet->common.generation == common_generation &&
                     !vnet->common.reset_in_progress;
    if (!common_current) {
        pthread_mutex_unlock(&vnet->common.transport_lock);
        return false;
    }

    if (!virtio_actor_begin_completion(&vnet->actor, actor_generation)) {
        pthread_mutex_unlock(&vnet->common.transport_lock);
        return false;
    }
    return true;
}

static void virtio_net_end_actor_completion(virtio_net_state_t *vnet)
{
    if (!vnet || !vnet->actor_initialized)
        return;

    virtio_actor_end_completion(&vnet->actor);
    pthread_mutex_unlock(&vnet->common.transport_lock);
}

static void virtio_net_set_fail_for_actor(virtio_net_state_t *vnet,
                                          uint64_t actor_generation,
                                          uint64_t common_generation)
{
    if (!virtio_net_begin_actor_completion(vnet, actor_generation,
                                           common_generation))
        return;
    virtio_net_set_fail(vnet);
    virtio_net_end_actor_completion(vnet);
}

static bool virtio_net_iovs_have_bytes(const struct virtq_iov *iov,
                                       size_t count,
                                       guest_size_t bytes)
{
    guest_size_t total = 0;

    for (size_t i = 0; i < count; i++) {
        if (iov[i].len > UINT64_MAX - total)
            return true;
        total += iov[i].len;
        if (total >= bytes)
            return true;
    }
    return false;
}

static bool virtio_net_read_iovs(virtio_net_state_t *vnet,
                                 const struct virtq_iov *iov,
                                 size_t count,
                                 guest_size_t skip,
                                 void *dst,
                                 guest_size_t len)
{
    uint8_t *out = dst;

    for (size_t i = 0; i < count && len > 0; i++) {
        guest_paddr_t addr = iov[i].addr;
        guest_size_t iov_len = iov[i].len;
        guest_size_t n;

        if (skip >= iov_len) {
            skip -= iov_len;
            continue;
        }

        addr += skip;
        iov_len -= skip;
        skip = 0;
        n = MIN(iov_len, len);
        if (!ram_dma_read(vnet->common.dma, addr, out, n))
            return false;
        out += n;
        len -= n;
    }
    return len == 0;
}

static bool virtio_net_write_iovs(virtio_net_state_t *vnet,
                                  const struct virtq_iov *iov,
                                  size_t count,
                                  guest_size_t skip,
                                  const void *src,
                                  guest_size_t len)
{
    const uint8_t *in = src;

    for (size_t i = 0; i < count && len > 0; i++) {
        guest_paddr_t addr = iov[i].addr;
        guest_size_t iov_len = iov[i].len;
        guest_size_t n;

        if (skip >= iov_len) {
            skip -= iov_len;
            continue;
        }

        addr += skip;
        iov_len -= skip;
        skip = 0;
        n = MIN(iov_len, len);
        if (!ram_dma_write(vnet->common.dma, addr, in, n))
            return false;
        in += n;
        len -= n;
    }
    return len == 0;
}

static bool virtio_net_guest_ptr(virtio_net_state_t *vnet,
                                 guest_paddr_t addr,
                                 guest_size_t len,
                                 void **ptr)
{
    ram_dma_t *dma;

    if (!vnet || !ptr)
        return false;
    dma = vnet->common.dma;
    if (!dma || !dma->words)
        return false;
    if (len == 0) {
        if (addr > dma->byte_size)
            return false;
    } else {
        if (addr >= dma->byte_size || len > dma->byte_size - addr)
            return false;
    }

    *ptr = (void *) ((uintptr_t) dma->words + (uintptr_t) addr);
    return true;
}

static int virtio_net_tx_build_iovs(virtio_net_state_t *vnet,
                                    const struct virtq_chain *chain,
                                    struct iovec *iovs,
                                    size_t *iov_count)
{
    guest_size_t skip = virtio_net_header_len(vnet);
    size_t out = 0;

    if (!virtio_net_iovs_have_bytes(chain->readable, chain->readable_count,
                                    skip))
        return -EINVAL;

    for (size_t i = 0; i < chain->readable_count; i++) {
        guest_paddr_t addr = chain->readable[i].addr;
        guest_size_t len = chain->readable[i].len;
        void *base = NULL;

        if (skip >= len) {
            skip -= len;
            continue;
        }

        addr += skip;
        len -= skip;
        skip = 0;
        if (len == 0)
            continue;
        if (out >= *iov_count)
            return -ENOSPC;
        if (!virtio_net_guest_ptr(vnet, addr, len, &base))
            return -EFAULT;
        iovs[out].iov_base = base;
        iovs[out].iov_len = len;
        out++;
    }

    *iov_count = out;
    return 0;
}

static ssize_t virtio_net_retrying_writev(int fd,
                                          const struct iovec *iov,
                                          int iovcnt)
{
    for (;;) {
        ssize_t ret = writev(fd, iov, iovcnt);

        if (ret >= 0)
            return ret;
        if (errno != EINTR)
            return ret;
    }
}

static ssize_t virtio_net_retrying_read(int fd, void *buf, size_t len)
{
    for (;;) {
        ssize_t ret = read(fd, buf, len);

        if (ret >= 0)
            return ret;
        if (errno != EINTR)
            return ret;
    }
}

static int virtio_net_host_read(virtio_net_state_t *vnet,
                                uint8_t *buf,
                                size_t len,
                                ssize_t *packet_len)
{
    ssize_t ret = 0;

    *packet_len = 0;
    if (!vnet->peer.op)
        return 0;

    switch (vnet->peer.type) {
#if defined(__APPLE__)
    case NETDEV_IMPL_vmnet:
        ret = net_vmnet_read((net_vmnet_state_t *) vnet->peer.op, buf, len);
        break;
#else
    case NETDEV_IMPL_tap:
        ret = virtio_net_retrying_read(
            ((net_tap_options_t *) vnet->peer.op)->tap_fd, buf, len);
        break;
#endif
    case NETDEV_IMPL_user:
        ret = virtio_net_retrying_read(
            ((net_user_options_t *) vnet->peer.op)
                ->guest_to_host_channel[SLIRP_READ_SIDE],
            buf, len);
        break;
    default:
        return 0;
    }

    if (ret < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        virtio_net_set_queue_fd_ready(vnet, VNET_QUEUE_RX, false);
        return -EAGAIN;
    }
    if (ret < 0) {
        fprintf(stderr, "[VNET] could not read packet: %s\n", strerror(errno));
        return -EIO;
    }

    *packet_len = ret;
    return 0;
}

static int virtio_net_host_write(virtio_net_state_t *vnet,
                                 const struct iovec *iovs,
                                 size_t iov_count,
                                 ssize_t *written)
{
    ssize_t ret = 0;

    *written = 0;
    if (!vnet->peer.op)
        return 0;
    if (iov_count == 0)
        return 0;
    if (iov_count > INT32_MAX)
        return -EINVAL;

    switch (vnet->peer.type) {
#if defined(__APPLE__)
    case NETDEV_IMPL_vmnet:
        ret = net_vmnet_writev((net_vmnet_state_t *) vnet->peer.op, iovs,
                               iov_count);
        break;
#else
    case NETDEV_IMPL_tap:
        ret = virtio_net_retrying_writev(
            ((net_tap_options_t *) vnet->peer.op)->tap_fd, iovs,
            (int) iov_count);
        break;
#endif
    case NETDEV_IMPL_user: {
        net_user_options_t *usr = (net_user_options_t *) vnet->peer.op;

        ret = virtio_net_retrying_writev(
            usr->host_to_guest_channel[SLIRP_WRITE_SIDE], iovs,
            (int) iov_count);
        break;
    }
    default:
        return 0;
    }

    if (ret < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        virtio_net_set_queue_fd_ready(vnet, VNET_QUEUE_TX, false);
        return -EAGAIN;
    }
    if (ret < 0) {
        fprintf(stderr, "[VNET] could not write packet: %s\n", strerror(errno));
        return -EIO;
    }

    *written = ret;
    return 0;
}

static int virtio_net_process_rx_chain(virtio_net_state_t *vnet,
                                       const struct virtq_chain *chain,
                                       uint32_t *used_len)
{
    uint8_t header[VNET_V1_HEADER_LEN] = {0};
    uint8_t packet[VNET_PACKET_MAX];
    unsigned header_len = virtio_net_header_len(vnet);
    ssize_t packet_len = 0;
    int ret;

    *used_len = 0;
    if (chain->readable_count != 0 || chain->writable_count == 0)
        return -EINVAL;
    if (header_len > sizeof(header))
        return -EINVAL;
    if (!virtio_net_iovs_have_bytes(chain->writable, chain->writable_count,
                                    header_len))
        return -EINVAL;

    ret = virtio_net_host_read(vnet, packet, sizeof(packet), &packet_len);
    if (ret < 0)
        return ret;
    if (packet_len == 0)
        return -EAGAIN;
    if (!virtio_net_iovs_have_bytes(
            chain->writable, chain->writable_count,
            (guest_size_t) header_len + (guest_size_t) packet_len))
        return -ENOSPC;

    if (!virtio_net_write_iovs(vnet, chain->writable, chain->writable_count, 0,
                               header, header_len) ||
        !virtio_net_write_iovs(vnet, chain->writable, chain->writable_count,
                               header_len, packet, (guest_size_t) packet_len))
        return -EFAULT;

    *used_len = (uint32_t) ((size_t) header_len + (size_t) packet_len);
    return 0;
}

static int virtio_net_process_tx_chain(virtio_net_state_t *vnet,
                                       const struct virtq_chain *chain,
                                       uint32_t *used_len)
{
    struct iovec host_iovs[VNET_QUEUE_NUM_MAX];
    size_t host_iov_count = ARRAY_SIZE(host_iovs);
    uint8_t header[VNET_V1_HEADER_LEN];
    unsigned header_len = virtio_net_header_len(vnet);
    ssize_t written = 0;
    int ret;

    *used_len = 0;
    if (chain->writable_count != 0 || chain->readable_count == 0)
        return -EINVAL;
    if (header_len > sizeof(header))
        return -EINVAL;
    if (!virtio_net_read_iovs(vnet, chain->readable, chain->readable_count, 0,
                              header, header_len))
        return -EFAULT;

    ret = virtio_net_tx_build_iovs(vnet, chain, host_iovs, &host_iov_count);
    if (ret < 0)
        return ret;

    ret = virtio_net_host_write(vnet, host_iovs, host_iov_count, &written);
    if (ret < 0)
        return ret;

    return 0;
}

static int virtio_net_actor_drain_queue(void *opaque,
                                        struct virtio_actor *actor,
                                        uint16_t queue_index,
                                        uint64_t generation)
{
    virtio_net_state_t *vnet = opaque;
    struct virtq *queue;
    struct virtq_iov readable[VNET_QUEUE_NUM_MAX];
    struct virtq_iov writable[VNET_QUEUE_NUM_MAX];
    bool consumed = false;
    uint64_t common_generation = 0;

    if (!vnet || queue_index >= VNET_QUEUE_COUNT) {
        if (vnet)
            virtio_net_set_fail(vnet);
        return 0;
    }

    queue = &vnet->common.queues[queue_index];

    if (!virtio_net_actor_generation_current(vnet, actor, generation))
        return 0;
    if (!virtio_net_queue_ready_for_actor(vnet, queue, queue_index))
        return 0;
    if (!virtio_net_capture_common_generation(vnet, &common_generation))
        return 0;

    for (;;) {
        struct virtq_chain chain = {
            .readable = readable,
            .readable_capacity = ARRAY_SIZE(readable),
            .writable = writable,
            .writable_capacity = ARRAY_SIZE(writable),
        };
        uint16_t available = 0;
        uint32_t used_len = 0;
        int ret;

        if (!virtio_net_actor_generation_current(vnet, actor, generation))
            return 0;
        if (!virtio_net_common_generation_current(vnet, common_generation))
            return 0;
        if (!virtio_net_queue_fd_ready(vnet, queue_index))
            return 0;

        ret = virtio_net_queue_available(vnet, queue, &available);
        if (ret < 0) {
            virtio_net_set_fail_for_actor(vnet, generation, common_generation);
            return 0;
        }
        if (available == 0)
            break;

        ret = virtq_pop(vnet->common.dma, queue, &chain);
        if (!virtio_net_actor_generation_current(vnet, actor, generation))
            return 0;
        if (!virtio_net_common_generation_current(vnet, common_generation))
            return 0;
        if (ret < 0) {
            virtio_net_set_fail_for_actor(vnet, generation, common_generation);
            return 0;
        }
        if (ret == 0)
            break;

        if (queue_index == VNET_QUEUE_RX)
            ret = virtio_net_process_rx_chain(vnet, &chain, &used_len);
        else
            ret = virtio_net_process_tx_chain(vnet, &chain, &used_len);

        if (!virtio_net_actor_generation_current(vnet, actor, generation))
            return 0;
        if (!virtio_net_common_generation_current(vnet, common_generation))
            return 0;
        if (ret == -EAGAIN) {
            queue->last_avail--;
            return 0;
        }
        if (ret < 0) {
            virtio_net_set_fail_for_actor(vnet, generation, common_generation);
            return 0;
        }

        if (!virtio_net_begin_actor_completion(vnet, generation,
                                               common_generation))
            return 0;
        ret = virtq_add_used(vnet->common.dma, queue, chain.head, used_len);
        if (ret < 0) {
            virtio_net_set_fail(vnet);
            virtio_net_end_actor_completion(vnet);
            return 0;
        }
        virtio_net_end_actor_completion(vnet);
        consumed = true;
    }

    if (consumed && virtio_net_begin_actor_completion(vnet, generation,
                                                      common_generation)) {
        if (!virtq_interrupt_suppressed(vnet->common.dma, queue))
            virtio_irq_trigger(&vnet->common.irq, VIRTIO_INT__USED_RING);
        virtio_net_end_actor_completion(vnet);
    }
    return 0;
}

static bool virtio_net_actor_queue_has_work(void *opaque,
                                            struct virtio_actor *actor,
                                            uint16_t queue_index,
                                            uint64_t generation)
{
    virtio_net_state_t *vnet = opaque;
    uint16_t available = 0;
    uint64_t common_generation = 0;
    int ret;

    if (!vnet || queue_index >= VNET_QUEUE_COUNT)
        return false;
    if (!virtio_net_actor_generation_current(vnet, actor, generation))
        return false;
    if (!virtio_net_queue_ready_for_actor(
            vnet, &vnet->common.queues[queue_index], queue_index))
        return false;
    if (!virtio_net_capture_common_generation(vnet, &common_generation))
        return false;

    ret = virtio_net_queue_available(vnet, &vnet->common.queues[queue_index],
                                     &available);
    if (ret < 0) {
        virtio_net_set_fail_for_actor(vnet, generation, common_generation);
        return false;
    }
    return available != 0;
}

static void virtio_net_actor_failed(void *opaque,
                                    struct virtio_actor *actor UNUSED)
{
    virtio_net_state_t *vnet = opaque;

    if (vnet)
        virtio_net_set_fail(vnet);
}

static const struct virtio_actor_ops virtio_net_actor_ops = {
    .drain_queue = virtio_net_actor_drain_queue,
    .queue_has_work = virtio_net_actor_queue_has_work,
    .on_failed = virtio_net_actor_failed,
};

static int virtio_net_activate(void *opaque,
                               const struct virtio_activation_context *ctx)
{
    virtio_net_state_t *vnet = opaque;
    int ret;

    if (!vnet || !vnet->actor_initialized)
        return -EINVAL;

    if (vnet->priv && ctx && ctx->common)
        atomic_store_explicit(
            &VNET_PRIV(vnet)->header_len,
            virtio_net_features_header_len(ctx->common->driver_features),
            memory_order_release);

    ret = virtio_actor_start(&vnet->actor);
    if (ret < 0 && ret != -EALREADY)
        return ret;

    ret = virtio_actor_enter_configuring(&vnet->actor);
    if (ret < 0)
        return ret;
    return virtio_actor_activate(&vnet->actor);
}

static int virtio_net_prepare_reset(void *opaque,
                                    uint64_t old_generation,
                                    uint64_t new_generation)
{
    virtio_net_state_t *vnet = opaque;

    (void) old_generation;
    (void) new_generation;

    if (!vnet || !vnet->actor_initialized)
        return 0;
    return virtio_actor_reset(&vnet->actor);
}

static int virtio_net_reset(void *opaque,
                            uint64_t old_generation,
                            uint64_t new_generation)
{
    virtio_net_state_t *vnet = opaque;

    (void) old_generation;
    (void) new_generation;

    if (!vnet)
        return 0;

    virtio_net_set_queue_fd_ready(vnet, VNET_QUEUE_RX, false);
    virtio_net_set_queue_fd_ready(vnet, VNET_QUEUE_TX, false);
    if (vnet->priv)
        atomic_store_explicit(&VNET_PRIV(vnet)->header_len,
                              VNET_LEGACY_HEADER_LEN, memory_order_release);
    return 0;
}

static int virtio_net_notify_queue(void *opaque,
                                   uint16_t queue_index,
                                   uint64_t generation)
{
    virtio_net_state_t *vnet = opaque;
    int ret;

    (void) generation;

    if (!vnet || queue_index >= VNET_QUEUE_COUNT) {
        if (vnet)
            virtio_net_set_fail(vnet);
        return -EINVAL;
    }

    ret = virtio_actor_notify_queue(&vnet->actor, queue_index);
    if (ret == 0 || ret == -EAGAIN)
        return 0;

    virtio_net_set_fail(vnet);
    return ret;
}

static const struct virtio_device_ops virtio_net_ops = {
    .activate = virtio_net_activate,
    .prepare_reset = virtio_net_prepare_reset,
    .reset = virtio_net_reset,
    .notify_queue = virtio_net_notify_queue,
    .read_config = virtio_net_read_config,
    .write_config = virtio_net_write_config,
};

static bool virtio_net_load_width_bytes(uint8_t width, size_t *access_size)
{
    switch (width) {
    case RV_MEM_LW:
        *access_size = 4;
        return true;
    case RV_MEM_LBU:
    case RV_MEM_LB:
        *access_size = 1;
        return true;
    case RV_MEM_LHU:
    case RV_MEM_LH:
        *access_size = 2;
        return true;
    default:
        return false;
    }
}

static bool virtio_net_store_width_bytes(uint8_t width, size_t *access_size)
{
    switch (width) {
    case RV_MEM_SW:
        *access_size = 4;
        return true;
    case RV_MEM_SB:
        *access_size = 1;
        return true;
    case RV_MEM_SH:
        *access_size = 2;
        return true;
    default:
        return false;
    }
}

static bool virtio_net_is_config_access(uint32_t addr, size_t access_size)
{
    const uint32_t base = VIRTIO_Config << 2;
    const uint32_t end = base + (uint32_t) sizeof(struct virtio_net_config);

    if (access_size == 0 || addr < base || addr >= end)
        return false;
    return access_size <= end - addr;
}

void virtio_net_read(hart_t *vm,
                     virtio_net_state_t *vnet,
                     uint32_t addr,
                     uint8_t width,
                     uint32_t *value)
{
    size_t access_size = 0;
    bool is_cfg;
    int ret;

    if (!virtio_net_load_width_bytes(width, &access_size)) {
        vm_set_exception(vm, RV_EXC_ILLEGAL_INSN, 0);
        return;
    }

    is_cfg = virtio_net_is_config_access(addr, access_size);
    if (addr >= (VIRTIO_Config << 2) && !is_cfg) {
        vm_set_exception(vm, RV_EXC_LOAD_FAULT, vm->exc_val);
        return;
    }

    if (!is_cfg) {
        if (access_size != 4 || (addr & 0x3)) {
            vm_set_exception(vm, RV_EXC_LOAD_MISALIGN, vm->exc_val);
            return;
        }
    } else if (addr & (access_size - 1)) {
        vm_set_exception(vm, RV_EXC_LOAD_MISALIGN, vm->exc_val);
        return;
    }

    ret = virtio_mmio_read(&vnet->common, addr, (uint8_t) access_size, value);
    if (ret < 0)
        vm_set_exception(vm, RV_EXC_LOAD_FAULT, vm->exc_val);
}

void virtio_net_write(hart_t *vm,
                      virtio_net_state_t *vnet,
                      uint32_t addr,
                      uint8_t width,
                      uint32_t value)
{
    size_t access_size = 0;
    bool is_cfg;
    int ret;

    if (!virtio_net_store_width_bytes(width, &access_size)) {
        vm_set_exception(vm, RV_EXC_ILLEGAL_INSN, 0);
        return;
    }

    is_cfg = virtio_net_is_config_access(addr, access_size);
    if (addr >= (VIRTIO_Config << 2) && !is_cfg) {
        vm_set_exception(vm, RV_EXC_STORE_FAULT, vm->exc_val);
        return;
    }

    if (!is_cfg) {
        if (access_size != 4 || (addr & 0x3)) {
            vm_set_exception(vm, RV_EXC_STORE_MISALIGN, vm->exc_val);
            return;
        }
    } else if (addr & (access_size - 1)) {
        vm_set_exception(vm, RV_EXC_STORE_MISALIGN, vm->exc_val);
        return;
    }

    ret = virtio_mmio_write(&vnet->common, addr, (uint8_t) access_size, value);
    if (ret < 0)
        vm_set_exception(vm, RV_EXC_STORE_FAULT, vm->exc_val);
}

bool virtio_net_irq_pending(virtio_net_state_t *vnet)
{
    return vnet && virtio_irq_read_status(&vnet->common.irq) != 0;
}

static void virtio_net_notify_if_active(virtio_net_state_t *vnet,
                                        uint16_t queue_index)
{
    int ret;

    if (!vnet || !vnet->actor_initialized)
        return;

    ret = virtio_actor_notify_queue(&vnet->actor, queue_index);
    if (ret < 0 && ret != -EAGAIN)
        virtio_net_set_fail(vnet);
}

static void virtio_net_poll_internal_rx(virtio_net_state_t *vnet,
                                        net_user_options_t *usr)
{
    struct pollfd rx = {usr->guest_to_host_channel[SLIRP_READ_SIDE], POLLIN, 0};

    if (poll(&rx, 1, 0) > 0 && (rx.revents & POLLIN)) {
        virtio_net_set_queue_fd_ready(vnet, VNET_QUEUE_RX, true);
        virtio_net_notify_if_active(vnet, VNET_QUEUE_RX);
    }
}

static void virtio_net_poll_internal_tx_ready(virtio_net_state_t *vnet,
                                              net_user_options_t *usr)
{
    struct pollfd tx = {usr->host_to_guest_channel[SLIRP_WRITE_SIDE], POLLOUT,
                        0};

    if (poll(&tx, 1, 0) > 0 && (tx.revents & POLLOUT)) {
        virtio_net_set_queue_fd_ready(vnet, VNET_QUEUE_TX, true);
        virtio_net_notify_if_active(vnet, VNET_QUEUE_TX);
    }
}

static void virtio_net_pump_user_slirp(virtio_net_state_t *vnet,
                                       net_user_options_t *usr)
{
    uint32_t timeout = 0;
    int pollout;

    if (!vnet || !usr || !usr->slirp || !usr->pfd || usr->pfd_size < 2)
        return;

    usr->pfd_len = 2;
    slirp_pollfds_fill_socket(usr->slirp, &timeout, semu_slirp_add_poll_socket,
                              usr);
    pollout = poll(usr->pfd, usr->pfd_len, 0);

    if (usr->pfd_len > 1 && (usr->pfd[1].revents & POLLIN))
        (void) net_slirp_read(usr);

    slirp_pollfds_poll(usr->slirp, pollout <= 0, semu_slirp_get_revents, usr);

    virtio_net_poll_internal_rx(vnet, usr);
    virtio_net_poll_internal_tx_ready(vnet, usr);
}

static void virtio_net_pump_user_slirp_dynamic(virtio_net_state_t *vnet,
                                               net_user_options_t *usr)
{
    uint32_t timeout = 0;
    nfds_t dynamic_count;
    int pollout = 0;

    if (!vnet || !usr || !usr->slirp || !usr->pfd || usr->pfd_size < 2)
        return;

    usr->pfd_len = 2;
    slirp_pollfds_fill_socket(usr->slirp, &timeout, semu_slirp_add_poll_socket,
                              usr);
    if (usr->pfd_len < 2)
        return;

    for (int i = 2; i < usr->pfd_len; i++)
        usr->pfd[i].revents = 0;

    dynamic_count = (nfds_t) (usr->pfd_len - 2);
    if (dynamic_count > 0)
        pollout = poll(&usr->pfd[2], dynamic_count, 0);

    slirp_pollfds_poll(usr->slirp, pollout < 0, semu_slirp_get_revents, usr);
}

static semu_event_token_t virtio_net_event_token(semu_event_token_t token_base,
                                                 uint32_t event)
{
    return token_base + event;
}

static bool virtio_net_event_token_match(semu_event_token_t token,
                                         semu_event_token_t token_base,
                                         uint32_t *event)
{
    semu_event_token_t offset;

    if (token < token_base)
        return false;
    offset = token - token_base;
    if (offset >= VNET_EVENT_COUNT)
        return false;
    if (event)
        *event = offset;
    return true;
}

static int virtio_net_event_del_fd(struct semu_event_loop *loop, int fd)
{
    int ret;

    if (fd < 0)
        return 0;

    ret = semu_event_del_fd(loop, fd);
    if (ret == -ENOENT)
        return 0;
    return ret;
}

static int virtio_net_event_upsert_fd(struct semu_event_loop *loop,
                                      int fd,
                                      semu_event_token_t token,
                                      uint32_t events)
{
    int ret;

    if (fd < 0)
        return 0;
    if (events == 0)
        return virtio_net_event_del_fd(loop, fd);

    ret = semu_event_add_fd(loop, fd, token, events);
    if (ret == -EEXIST)
        ret = semu_event_mod_fd(loop, fd, token, events);
    return ret;
}

static uint32_t virtio_net_tx_event_mask(virtio_net_state_t *vnet)
{
    return virtio_net_queue_fd_ready(vnet, VNET_QUEUE_TX) ? 0
                                                          : SEMU_EVENT_WRITABLE;
}

static bool virtio_net_user_slirp_pollfds_available(
    const net_user_options_t *usr)
{
    return usr && usr->slirp && usr->pfd && usr->pfd_size >= 2;
}

static void virtio_net_user_refresh_dynamic_pollfds(net_user_options_t *usr)
{
    uint32_t timeout = 0;

    if (!virtio_net_user_slirp_pollfds_available(usr))
        return;

    usr->pfd_len = 2;
    slirp_pollfds_fill_socket(usr->slirp, &timeout, semu_slirp_add_poll_socket,
                              usr);
}

static uint32_t virtio_net_poll_events_to_semu(short events)
{
    uint32_t mask = 0;

    if (events & (POLLIN | POLLPRI))
        mask |= SEMU_EVENT_READABLE;
    if (events & POLLOUT)
        mask |= SEMU_EVENT_WRITABLE;
    if (events & (POLLERR | POLLHUP | POLLNVAL))
        mask |= SEMU_EVENT_ERROR;
    return mask;
}

static bool virtio_net_user_dynamic_fd_current(const net_user_options_t *usr,
                                               int fd)
{
    if (!usr || !usr->pfd || fd < 0 || usr->pfd_len <= 2)
        return false;

    for (int i = 2; i < usr->pfd_len; i++) {
        if (usr->pfd[i].fd == fd)
            return true;
    }
    return false;
}

static int virtio_net_event_del_token(struct semu_event_loop *loop,
                                      semu_event_token_t token)
{
    size_t i = 0;

    while (i < loop->count) {
        int ret;

        if (loop->tokens[i] != token) {
            i++;
            continue;
        }

        ret = virtio_net_event_del_fd(loop, loop->fds[i]);
        if (ret < 0)
            return ret;
    }
    return 0;
}

static int virtio_net_event_prune_user_dynamic(struct semu_event_loop *loop,
                                               const net_user_options_t *usr,
                                               semu_event_token_t token_base)
{
    semu_event_token_t token =
        virtio_net_event_token(token_base, VNET_EVENT_USER_DYNAMIC);
    size_t i = 0;

    while (i < loop->count) {
        int ret;

        if (loop->tokens[i] != token ||
            virtio_net_user_dynamic_fd_current(usr, loop->fds[i])) {
            i++;
            continue;
        }

        ret = virtio_net_event_del_fd(loop, loop->fds[i]);
        if (ret < 0)
            return ret;
    }
    return 0;
}

static int virtio_net_event_sync_user_dynamic(struct semu_event_loop *loop,
                                              net_user_options_t *usr,
                                              semu_event_token_t token_base)
{
    semu_event_token_t token =
        virtio_net_event_token(token_base, VNET_EVENT_USER_DYNAMIC);
    int ret;

    if (virtio_net_user_slirp_pollfds_available(usr))
        virtio_net_user_refresh_dynamic_pollfds(usr);
    else if (usr && usr->pfd && usr->pfd_size >= 2)
        usr->pfd_len = 2;

    ret = virtio_net_event_prune_user_dynamic(loop, usr, token_base);
    if (ret < 0)
        return ret;
    if (!virtio_net_user_slirp_pollfds_available(usr))
        return 0;

    for (int i = 2; i < usr->pfd_len; i++) {
        ret = virtio_net_event_upsert_fd(
            loop, usr->pfd[i].fd, token,
            virtio_net_poll_events_to_semu(usr->pfd[i].events));
        if (ret == -ENOSPC) {
            /* Dynamic Slirp sockets are opportunistic subscriptions. If the
             * fixed-capacity event loop is full, keep the VM running and let
             * the periodic Slirp pump cover the remaining transient sockets.
             */
            return 0;
        }
        if (ret == -EBADF) {
            (void) virtio_net_event_del_fd(loop, usr->pfd[i].fd);
            continue;
        }
        if (ret < 0)
            return ret;
    }
    return 0;
}

int virtio_net_event_sync(virtio_net_state_t *vnet,
                          struct semu_event_loop *loop,
                          semu_event_token_t token_base)
{
    int ret;

    if (!vnet || !loop)
        return -EINVAL;
    if (!vnet->peer.op)
        return 0;

    switch (vnet->peer.type) {
#if defined(__APPLE__)
    case NETDEV_IMPL_vmnet: {
        net_vmnet_state_t *vmnet = (net_vmnet_state_t *) vnet->peer.op;

        virtio_net_set_queue_fd_ready(vnet, VNET_QUEUE_TX, true);
        virtio_net_notify_if_active(vnet, VNET_QUEUE_TX);
        return virtio_net_event_upsert_fd(
            loop, net_vmnet_get_fd(vmnet),
            virtio_net_event_token(token_base, VNET_EVENT_BACKEND),
            SEMU_EVENT_READABLE);
    }
#else
    case NETDEV_IMPL_tap: {
        net_tap_options_t *tap = (net_tap_options_t *) vnet->peer.op;
        uint32_t events = SEMU_EVENT_READABLE | virtio_net_tx_event_mask(vnet);

        return virtio_net_event_upsert_fd(
            loop, tap->tap_fd,
            virtio_net_event_token(token_base, VNET_EVENT_BACKEND), events);
    }
#endif
    case NETDEV_IMPL_user: {
        net_user_options_t *usr = (net_user_options_t *) vnet->peer.op;

        ret = virtio_net_event_upsert_fd(
            loop, usr->guest_to_host_channel[SLIRP_READ_SIDE],
            virtio_net_event_token(token_base, VNET_EVENT_USER_RX),
            SEMU_EVENT_READABLE);
        if (ret < 0)
            return ret;

        ret = virtio_net_event_upsert_fd(
            loop, usr->host_to_guest_channel[SLIRP_READ_SIDE],
            virtio_net_event_token(token_base, VNET_EVENT_USER_SLIRP_IN),
            SEMU_EVENT_READABLE);
        if (ret < 0)
            return ret;

        ret = virtio_net_event_upsert_fd(
            loop, usr->host_to_guest_channel[SLIRP_WRITE_SIDE],
            virtio_net_event_token(token_base, VNET_EVENT_USER_TX),
            virtio_net_tx_event_mask(vnet));
        if (ret < 0)
            return ret;

        return virtio_net_event_sync_user_dynamic(loop, usr, token_base);
    }
    default:
        return 0;
    }
}

bool virtio_net_event_handle(virtio_net_state_t *vnet,
                             const struct semu_event *event,
                             semu_event_token_t token_base)
{
    uint32_t token;
    uint32_t ready =
        SEMU_EVENT_READABLE | SEMU_EVENT_WRITABLE | SEMU_EVENT_ERROR;

    if (!vnet || !event || !vnet->peer.op ||
        !virtio_net_event_token_match(event->token, token_base, &token))
        return false;
    if ((event->events & ready) == 0)
        return true;

    switch (vnet->peer.type) {
#if defined(__APPLE__)
    case NETDEV_IMPL_vmnet:
        if (token != VNET_EVENT_BACKEND)
            return false;
        if (event->events & (SEMU_EVENT_READABLE | SEMU_EVENT_ERROR)) {
            virtio_net_set_queue_fd_ready(vnet, VNET_QUEUE_RX, true);
            virtio_net_notify_if_active(vnet, VNET_QUEUE_RX);
        }
        return true;
#else
    case NETDEV_IMPL_tap:
        if (token != VNET_EVENT_BACKEND)
            return false;
        if (event->events & (SEMU_EVENT_READABLE | SEMU_EVENT_ERROR)) {
            virtio_net_set_queue_fd_ready(vnet, VNET_QUEUE_RX, true);
            virtio_net_notify_if_active(vnet, VNET_QUEUE_RX);
        }
        if (event->events & (SEMU_EVENT_WRITABLE | SEMU_EVENT_ERROR)) {
            virtio_net_set_queue_fd_ready(vnet, VNET_QUEUE_TX, true);
            virtio_net_notify_if_active(vnet, VNET_QUEUE_TX);
        }
        return true;
#endif
    case NETDEV_IMPL_user: {
        net_user_options_t *usr = (net_user_options_t *) vnet->peer.op;

        switch (token) {
        case VNET_EVENT_USER_RX:
            if (event->events & (SEMU_EVENT_READABLE | SEMU_EVENT_ERROR)) {
                virtio_net_set_queue_fd_ready(vnet, VNET_QUEUE_RX, true);
                virtio_net_notify_if_active(vnet, VNET_QUEUE_RX);
            }
            return true;
        case VNET_EVENT_USER_SLIRP_IN:
            if (event->events & (SEMU_EVENT_READABLE | SEMU_EVENT_ERROR)) {
                if (usr->slirp && net_slirp_read(usr) < 0)
                    virtio_net_set_fail(vnet);
                virtio_net_pump_user_slirp_dynamic(vnet, usr);
            }
            return true;
        case VNET_EVENT_USER_TX:
            if (event->events & (SEMU_EVENT_WRITABLE | SEMU_EVENT_ERROR)) {
                virtio_net_set_queue_fd_ready(vnet, VNET_QUEUE_TX, true);
                virtio_net_notify_if_active(vnet, VNET_QUEUE_TX);
            }
            return true;
        case VNET_EVENT_USER_DYNAMIC:
            virtio_net_pump_user_slirp_dynamic(vnet, usr);
            return true;
        default:
            return false;
        }
    }
    default:
        return false;
    }
}

void virtio_net_event_poll_fallback(virtio_net_state_t *vnet)
{
    if (!vnet || !vnet->peer.op || vnet->peer.type != NETDEV_IMPL_user)
        return;

    /* Dynamic Slirp sockets are subscribed through semu_event_loop during sync.
     * Keep this periodic pump for libslirp timer bookkeeping and socket-list
     * churn that can occur without a host fd readiness edge.
     */
    virtio_net_pump_user_slirp_dynamic(vnet,
                                       (net_user_options_t *) vnet->peer.op);
}

void virtio_net_event_unregister(virtio_net_state_t *vnet,
                                 struct semu_event_loop *loop,
                                 semu_event_token_t token_base)
{
    if (!vnet || !loop || !vnet->peer.op)
        return;

    switch (vnet->peer.type) {
#if defined(__APPLE__)
    case NETDEV_IMPL_vmnet:
        (void) virtio_net_event_del_fd(
            loop, net_vmnet_get_fd((net_vmnet_state_t *) vnet->peer.op));
        break;
#else
    case NETDEV_IMPL_tap:
        (void) virtio_net_event_del_fd(
            loop, ((net_tap_options_t *) vnet->peer.op)->tap_fd);
        break;
#endif
    case NETDEV_IMPL_user: {
        net_user_options_t *usr = (net_user_options_t *) vnet->peer.op;

        (void) virtio_net_event_del_fd(
            loop, usr->guest_to_host_channel[SLIRP_READ_SIDE]);
        (void) virtio_net_event_del_fd(
            loop, usr->host_to_guest_channel[SLIRP_READ_SIDE]);
        (void) virtio_net_event_del_fd(
            loop, usr->host_to_guest_channel[SLIRP_WRITE_SIDE]);
        (void) virtio_net_event_del_token(
            loop, virtio_net_event_token(token_base, VNET_EVENT_USER_DYNAMIC));
        break;
    }
    default:
        break;
    }
}

void virtio_net_recv_from_peer(void *peer)
{
    virtio_net_state_t *vnet = (virtio_net_state_t *) peer;

    if (!vnet)
        return;

    virtio_net_set_queue_fd_ready(vnet, VNET_QUEUE_RX, true);
    virtio_net_notify_if_active(vnet, VNET_QUEUE_RX);
}

void virtio_net_refresh_queue(virtio_net_state_t *vnet)
{
    unsigned status;

    if (!vnet)
        return;

    status = virtio_net_status_load(vnet);
    if (!(status & VIRTIO_STATUS__DRIVER_OK) ||
        (status & VIRTIO_STATUS__DEVICE_NEEDS_RESET))
        return;
    if (!vnet->peer.op)
        return;

    switch (vnet->peer.type) {
#if defined(__APPLE__)
    case NETDEV_IMPL_vmnet: {
        net_vmnet_state_t *vmnet = (net_vmnet_state_t *) vnet->peer.op;
        struct pollfd pfd = {net_vmnet_get_fd(vmnet), POLLIN, 0};

        (void) poll(&pfd, 1, 0);
        if (pfd.revents & POLLIN) {
            virtio_net_set_queue_fd_ready(vnet, VNET_QUEUE_RX, true);
            virtio_net_notify_if_active(vnet, VNET_QUEUE_RX);
        }
        virtio_net_set_queue_fd_ready(vnet, VNET_QUEUE_TX, true);
        virtio_net_notify_if_active(vnet, VNET_QUEUE_TX);
        break;
    }
#else
    case NETDEV_IMPL_tap: {
        net_tap_options_t *tap = (net_tap_options_t *) vnet->peer.op;
        struct pollfd pfd = {tap->tap_fd, POLLIN | POLLOUT, 0};

        (void) poll(&pfd, 1, 0);
        if (pfd.revents & POLLIN) {
            virtio_net_set_queue_fd_ready(vnet, VNET_QUEUE_RX, true);
            virtio_net_notify_if_active(vnet, VNET_QUEUE_RX);
        }
        if (pfd.revents & POLLOUT) {
            virtio_net_set_queue_fd_ready(vnet, VNET_QUEUE_TX, true);
            virtio_net_notify_if_active(vnet, VNET_QUEUE_TX);
        }
        break;
    }
#endif
    case NETDEV_IMPL_user: {
        net_user_options_t *usr = (net_user_options_t *) vnet->peer.op;

        virtio_net_pump_user_slirp(vnet, usr);
        break;
    }
    default:
        break;
    }
}

bool virtio_net_init(virtio_net_state_t *vnet,
                     emu_state_t *emu,
                     const char *name)
{
    static const uint16_t queue_max_sizes[] = {
        [VNET_QUEUE_RX] = VNET_QUEUE_NUM_MAX,
        [VNET_QUEUE_TX] = VNET_QUEUE_NUM_MAX,
    };
    struct virtio_device_common_config common_config;
    struct virtio_net_priv *priv;

    if (!vnet || !emu) {
        fprintf(stderr, "Failed to initialize virtio-net common device.\n");
        return false;
    }

    memset(vnet, 0, sizeof(*vnet));
    vnet->ram = emu->ram;

    priv = calloc(1, sizeof(*priv));
    if (!priv) {
        fprintf(stderr, "Failed to allocate virtio-net config.\n");
        return false;
    }
    priv->config.status = 1;
    priv->config.max_virtqueue_pairs = 1;
    priv->config.mtu = 1500;
    atomic_init(&priv->header_len, VNET_LEGACY_HEADER_LEN);
    vnet->priv = priv;

    common_config = (struct virtio_device_common_config) {
        .emu = emu,
        .dma = &emu->ram_dma,
        .irq_source = SEMU_IRQ_SOURCE_VNET,
        .device_id = 1,
        .vendor_id = VIRTIO_VENDOR_ID,
        .device_features = VIRTIO_NET_F_VERSION_1,
        .required_features = VIRTIO_NET_F_VERSION_1,
        .queue_max_sizes = queue_max_sizes,
        .num_queues = ARRAY_SIZE(queue_max_sizes),
        .ops = &virtio_net_ops,
        .opaque = vnet,
    };

    if (virtio_device_common_init(&vnet->common, &common_config) < 0) {
        free(priv);
        vnet->priv = NULL;
        fprintf(stderr, "Failed to initialize virtio-net common device.\n");
        return false;
    }

    if (virtio_actor_init(&vnet->actor, &virtio_net_actor_ops, vnet,
                          ARRAY_SIZE(queue_max_sizes)) < 0) {
        virtio_device_common_destroy(&vnet->common);
        free(priv);
        vnet->priv = NULL;
        fprintf(stderr, "Failed to initialize virtio-net actor.\n");
        return false;
    }
    vnet->actor_initialized = true;

    if (name) {
        if (!netdev_init(&vnet->peer, name)) {
            virtio_net_destroy(vnet);
            fprintf(stderr, "Fail to init net device %s\n", name);
            return false;
        }
        VNET_PRIV(vnet)->peer_owned = true;
    }

#if defined(__APPLE__)
    if (vnet->peer.op && vnet->peer.type == NETDEV_IMPL_vmnet)
        virtio_net_set_queue_fd_ready(vnet, VNET_QUEUE_TX, true);
#endif

    return true;
}

void virtio_net_destroy(virtio_net_state_t *vnet)
{
    struct virtio_net_priv *priv;

    if (!vnet)
        return;

    priv = VNET_PRIV(vnet);

    if (vnet->actor_initialized) {
        virtio_actor_stop(&vnet->actor);
        virtio_actor_destroy(&vnet->actor);
        vnet->actor_initialized = false;
    }

    virtio_device_common_destroy(&vnet->common);

    if (priv && priv->peer_owned && vnet->peer.op) {
#if defined(__APPLE__)
        if (vnet->peer.type == NETDEV_IMPL_vmnet)
            net_vmnet_cleanup((net_vmnet_state_t *) vnet->peer.op);
#endif
        free(vnet->peer.op);
        vnet->peer.op = NULL;
    }

    free(priv);
    vnet->priv = NULL;
}
