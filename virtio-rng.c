#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

#define VIRTIO_RNG_F_VERSION_1 (UINT64_C(1) << 32)

#define VRNG_QUEUE_NUM_MAX 1024
#define VRNG_QUEUE 0
#define VRNG_CHUNK_SIZE 256

static int rng_fd = -1;

static inline unsigned virtio_rng_status_load(virtio_rng_state_t *vrng)
{
    return atomic_load_explicit(&vrng->common.status, memory_order_acquire);
}

static void virtio_rng_set_fail(virtio_rng_state_t *vrng)
{
    unsigned status = virtio_rng_status_load(vrng);

    virtio_device_common_set_needs_reset(&vrng->common);
    if (status & VIRTIO_STATUS__DRIVER_OK)
        virtio_irq_trigger(&vrng->common.irq, VIRTIO_INT__CONF_CHANGE);
}

static size_t virtio_rng_read_entropy(void *buf,
                                      size_t len,
                                      bool *permanent_failure)
{
    if (permanent_failure)
        *permanent_failure = false;

    for (;;) {
        ssize_t total = read(rng_fd, buf, len);

        if (total >= 0)
            return (size_t) total;

        if (errno == EINTR)
            continue;

        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            /* The actor does not subscribe to entropy fd readiness yet, so
             * publish a deterministic zero-byte completion and let the guest
             * requeue.
             */
            return 0;
        }

        if (permanent_failure)
            *permanent_failure = true;
        return 0;
    }
}

static int virtio_rng_write_entropy_iovs(virtio_rng_state_t *vrng,
                                         const struct virtq_chain *chain,
                                         uint32_t *written,
                                         bool *permanent_failure)
{
    uint8_t buf[VRNG_CHUNK_SIZE];

    *written = 0;
    *permanent_failure = false;

    for (size_t i = 0; i < chain->writable_count; i++) {
        guest_paddr_t addr = chain->writable[i].addr;
        guest_size_t remaining = chain->writable[i].len;

        while (remaining > 0) {
            size_t request = MIN((size_t) remaining, sizeof(buf));
            bool failed = false;
            size_t got = virtio_rng_read_entropy(buf, request, &failed);

            if (got > 0) {
                if (!ram_dma_write(vrng->common.dma, addr, buf, got))
                    return -EFAULT;
                addr += got;
                remaining -= got;
                *written += (uint32_t) got;
            }

            if (failed) {
                *permanent_failure = true;
                return 0;
            }
            if (got < request)
                return 0;
        }
    }

    return 0;
}

static bool virtio_rng_actor_generation_current(virtio_rng_state_t *vrng,
                                                struct virtio_actor *actor,
                                                uint64_t generation)
{
    (void) vrng;
    return actor && virtio_actor_generation(actor) == generation;
}

static bool virtio_rng_queue_ready_for_actor(virtio_rng_state_t *vrng,
                                             const struct virtq *queue)
{
    unsigned status;

    if (!vrng || !queue || !queue->ready)
        return false;

    status = virtio_rng_status_load(vrng);
    return !(status & VIRTIO_STATUS__DEVICE_NEEDS_RESET) &&
           (status & VIRTIO_STATUS__DRIVER_OK);
}

static int virtio_rng_queue_available(virtio_rng_state_t *vrng,
                                      struct virtq *queue,
                                      uint16_t *available)
{
    uint16_t avail_idx;
    uint16_t delta;

    if (!vrng || !queue || !queue->ready || !available)
        return -EINVAL;
    if (!ram_dma_read(vrng->common.dma, queue->driver_addr + 2, &avail_idx,
                      sizeof(avail_idx)))
        return -EFAULT;

    delta = (uint16_t) (avail_idx - queue->last_avail);
    if (delta > queue->queue_size)
        return -EINVAL;

    *available = delta;
    return 0;
}

static bool virtio_rng_common_generation_current(virtio_rng_state_t *vrng,
                                                 uint64_t generation)
{
    bool current;

    if (!vrng || !vrng->common.initialized)
        return false;

    pthread_mutex_lock(&vrng->common.transport_lock);
    current = vrng->common.generation == generation &&
              !vrng->common.reset_in_progress;
    pthread_mutex_unlock(&vrng->common.transport_lock);
    return current;
}

static bool virtio_rng_capture_common_generation(virtio_rng_state_t *vrng,
                                                 uint64_t *generation)
{
    unsigned status;
    bool current;

    if (!vrng || !vrng->common.initialized || !generation)
        return false;

    pthread_mutex_lock(&vrng->common.transport_lock);
    status = virtio_rng_status_load(vrng);
    current = !vrng->common.reset_in_progress &&
              (status & VIRTIO_STATUS__DRIVER_OK) &&
              !(status & VIRTIO_STATUS__DEVICE_NEEDS_RESET);
    if (current)
        *generation = vrng->common.generation;
    pthread_mutex_unlock(&vrng->common.transport_lock);
    return current;
}

static bool virtio_rng_begin_actor_completion(virtio_rng_state_t *vrng,
                                              uint64_t actor_generation,
                                              uint64_t common_generation)
{
    bool common_current;

    if (!vrng || !vrng->actor_initialized || !vrng->common.initialized)
        return false;

    pthread_mutex_lock(&vrng->common.transport_lock);
    common_current = vrng->common.generation == common_generation &&
                     !vrng->common.reset_in_progress;
    if (!common_current) {
        pthread_mutex_unlock(&vrng->common.transport_lock);
        return false;
    }

    if (!virtio_actor_begin_completion(&vrng->actor, actor_generation)) {
        pthread_mutex_unlock(&vrng->common.transport_lock);
        return false;
    }
    return true;
}

static void virtio_rng_end_actor_completion(virtio_rng_state_t *vrng)
{
    if (!vrng || !vrng->actor_initialized)
        return;

    virtio_actor_end_completion(&vrng->actor);
    pthread_mutex_unlock(&vrng->common.transport_lock);
}

static void virtio_rng_set_fail_for_actor(virtio_rng_state_t *vrng,
                                          uint64_t actor_generation,
                                          uint64_t common_generation)
{
    if (!virtio_rng_begin_actor_completion(vrng, actor_generation,
                                           common_generation))
        return;
    virtio_rng_set_fail(vrng);
    virtio_rng_end_actor_completion(vrng);
}

static int virtio_rng_actor_drain_queue(void *opaque,
                                        struct virtio_actor *actor,
                                        uint16_t queue_index,
                                        uint64_t generation)
{
    virtio_rng_state_t *vrng = opaque;
    struct virtq *queue;
    struct virtq_iov readable[VRNG_QUEUE_NUM_MAX];
    struct virtq_iov writable[VRNG_QUEUE_NUM_MAX];
    bool consumed = false;
    uint64_t common_generation = 0;

    if (!vrng || queue_index != VRNG_QUEUE) {
        if (vrng)
            virtio_rng_set_fail(vrng);
        return 0;
    }

    queue = &vrng->common.queues[VRNG_QUEUE];

    if (!virtio_rng_actor_generation_current(vrng, actor, generation))
        return 0;
    if (!virtio_rng_queue_ready_for_actor(vrng, queue))
        return 0;
    if (!virtio_rng_capture_common_generation(vrng, &common_generation))
        return 0;

    for (;;) {
        struct virtq_chain chain = {
            .readable = readable,
            .readable_capacity = ARRAY_SIZE(readable),
            .writable = writable,
            .writable_capacity = ARRAY_SIZE(writable),
        };
        bool permanent_failure = false;
        uint16_t available = 0;
        uint32_t written = 0;
        int ret;

        if (!virtio_rng_actor_generation_current(vrng, actor, generation))
            return 0;
        if (!virtio_rng_common_generation_current(vrng, common_generation))
            return 0;
        ret = virtio_rng_queue_available(vrng, queue, &available);
        if (ret < 0) {
            virtio_rng_set_fail_for_actor(vrng, generation, common_generation);
            return 0;
        }
        if (available == 0)
            break;

        ret = virtq_pop(vrng->common.dma, queue, &chain);
        if (!virtio_rng_actor_generation_current(vrng, actor, generation))
            return 0;
        if (!virtio_rng_common_generation_current(vrng, common_generation))
            return 0;
        if (ret < 0) {
            virtio_rng_set_fail_for_actor(vrng, generation, common_generation);
            return 0;
        }
        if (ret == 0)
            break;

        if (chain.readable_count != 0 || chain.writable_count == 0) {
            virtio_rng_set_fail_for_actor(vrng, generation, common_generation);
            return 0;
        }

        ret = virtio_rng_write_entropy_iovs(vrng, &chain, &written,
                                            &permanent_failure);
        if (!virtio_rng_actor_generation_current(vrng, actor, generation))
            return 0;
        if (!virtio_rng_common_generation_current(vrng, common_generation))
            return 0;
        if (ret < 0) {
            virtio_rng_set_fail_for_actor(vrng, generation, common_generation);
            return 0;
        }

        if (!virtio_rng_begin_actor_completion(vrng, generation,
                                               common_generation))
            return 0;
        if (permanent_failure)
            virtio_rng_set_fail(vrng);
        ret = virtq_add_used(vrng->common.dma, queue, chain.head, written);
        if (ret < 0) {
            virtio_rng_set_fail(vrng);
            virtio_rng_end_actor_completion(vrng);
            return 0;
        }
        virtio_rng_end_actor_completion(vrng);
        consumed = true;

        if (permanent_failure)
            break;
    }

    if (consumed && virtio_rng_begin_actor_completion(vrng, generation,
                                                      common_generation)) {
        if (!virtq_interrupt_suppressed(vrng->common.dma, queue))
            virtio_irq_trigger(&vrng->common.irq, VIRTIO_INT__USED_RING);
        virtio_rng_end_actor_completion(vrng);
    }
    return 0;
}

static bool virtio_rng_actor_queue_has_work(void *opaque,
                                            struct virtio_actor *actor,
                                            uint16_t queue_index,
                                            uint64_t generation)
{
    virtio_rng_state_t *vrng = opaque;
    uint16_t available = 0;

    if (!vrng || queue_index != VRNG_QUEUE)
        return false;
    if (!virtio_rng_actor_generation_current(vrng, actor, generation))
        return false;
    if (!virtio_rng_queue_ready_for_actor(vrng,
                                          &vrng->common.queues[VRNG_QUEUE]))
        return false;
    if (virtio_rng_queue_available(vrng, &vrng->common.queues[VRNG_QUEUE],
                                   &available) < 0)
        return false;
    return available != 0;
}

static void virtio_rng_actor_failed(void *opaque,
                                    struct virtio_actor *actor UNUSED)
{
    virtio_rng_state_t *vrng = opaque;

    if (vrng)
        virtio_rng_set_fail(vrng);
}

static const struct virtio_actor_ops virtio_rng_actor_ops = {
    .drain_queue = virtio_rng_actor_drain_queue,
    .queue_has_work = virtio_rng_actor_queue_has_work,
    .on_failed = virtio_rng_actor_failed,
};

static int virtio_rng_activate(void *opaque,
                               const struct virtio_activation_context *ctx)
{
    virtio_rng_state_t *vrng = opaque;
    int ret;

    (void) ctx;

    if (!vrng || !vrng->actor_initialized)
        return -EINVAL;

    ret = virtio_actor_start(&vrng->actor);
    if (ret < 0 && ret != -EALREADY)
        return ret;

    ret = virtio_actor_enter_configuring(&vrng->actor);
    if (ret < 0)
        return ret;
    return virtio_actor_activate(&vrng->actor);
}

static int virtio_rng_prepare_reset(void *opaque,
                                    uint64_t old_generation,
                                    uint64_t new_generation)
{
    virtio_rng_state_t *vrng = opaque;

    (void) old_generation;
    (void) new_generation;

    if (!vrng || !vrng->actor_initialized)
        return 0;
    return virtio_actor_reset(&vrng->actor);
}

static int virtio_rng_reset(void *opaque,
                            uint64_t old_generation,
                            uint64_t new_generation)
{
    (void) opaque;
    (void) old_generation;
    (void) new_generation;
    return 0;
}

static int virtio_rng_notify_queue(void *opaque,
                                   uint16_t queue_index,
                                   uint64_t generation)
{
    virtio_rng_state_t *vrng = opaque;
    int ret;

    (void) generation;

    if (!vrng || queue_index != VRNG_QUEUE) {
        if (vrng)
            virtio_rng_set_fail(vrng);
        return -EINVAL;
    }

    ret = virtio_actor_notify_queue(&vrng->actor, queue_index);
    if (ret == 0 || ret == -EAGAIN)
        return 0;

    virtio_rng_set_fail(vrng);
    return ret;
}

static const struct virtio_device_ops virtio_rng_ops = {
    .activate = virtio_rng_activate,
    .prepare_reset = virtio_rng_prepare_reset,
    .reset = virtio_rng_reset,
    .notify_queue = virtio_rng_notify_queue,
};

void virtio_rng_read(hart_t *vm,
                     virtio_rng_state_t *vrng,
                     uint32_t addr,
                     uint8_t width,
                     uint32_t *value)
{
    int ret;

    switch (width) {
    case RV_MEM_LW:
        if (addr & 0x3) {
            vm_set_exception(vm, RV_EXC_LOAD_MISALIGN, vm->exc_val);
            return;
        }
        if (addr >= (VIRTIO_Config << 2)) {
            vm_set_exception(vm, RV_EXC_LOAD_FAULT, vm->exc_val);
            return;
        }
        ret = virtio_mmio_read(&vrng->common, addr, sizeof(uint32_t), value);
        if (ret < 0)
            vm_set_exception(vm, RV_EXC_LOAD_FAULT, vm->exc_val);
        break;
    case RV_MEM_LBU:
    case RV_MEM_LB:
    case RV_MEM_LHU:
    case RV_MEM_LH:
        vm_set_exception(vm, RV_EXC_LOAD_MISALIGN, vm->exc_val);
        return;
    default:
        vm_set_exception(vm, RV_EXC_ILLEGAL_INSN, 0);
        return;
    }
}

void virtio_rng_write(hart_t *vm,
                      virtio_rng_state_t *vrng,
                      uint32_t addr,
                      uint8_t width,
                      uint32_t value)
{
    int ret;

    switch (width) {
    case RV_MEM_SW:
        if (addr & 0x3) {
            vm_set_exception(vm, RV_EXC_STORE_MISALIGN, vm->exc_val);
            return;
        }
        if (addr >= (VIRTIO_Config << 2)) {
            vm_set_exception(vm, RV_EXC_STORE_FAULT, vm->exc_val);
            return;
        }
        ret = virtio_mmio_write(&vrng->common, addr, sizeof(uint32_t), value);
        if (ret < 0)
            vm_set_exception(vm, RV_EXC_STORE_FAULT, vm->exc_val);
        break;
    case RV_MEM_SB:
    case RV_MEM_SH:
        vm_set_exception(vm, RV_EXC_STORE_MISALIGN, vm->exc_val);
        return;
    default:
        vm_set_exception(vm, RV_EXC_ILLEGAL_INSN, 0);
        return;
    }
}

bool virtio_rng_irq_pending(virtio_rng_state_t *vrng)
{
    return virtio_irq_read_status(&vrng->common.irq) != 0;
}

static void virtio_rng_open_entropy_source(void)
{
    if (rng_fd >= 0)
        return;

    rng_fd = open("/dev/random", O_RDONLY | O_NONBLOCK);
    if (rng_fd < 0) {
        fprintf(stderr, "Could not open /dev/random\n");
        exit(2);
    }
}

void virtio_rng_init(virtio_rng_state_t *vrng, emu_state_t *emu)
{
    static const uint16_t queue_max_sizes[] = {
        [VRNG_QUEUE] = VRNG_QUEUE_NUM_MAX,
    };
    struct virtio_device_common_config config;

    if (!vrng || !emu) {
        fprintf(stderr, "Failed to initialize virtio-rng common device.\n");
        exit(2);
    }

    memset(vrng, 0, sizeof(*vrng));
    vrng->ram = emu->ram;
    virtio_rng_open_entropy_source();

    config = (struct virtio_device_common_config) {
        .emu = emu,
        .dma = &emu->ram_dma,
        .irq_source = SEMU_IRQ_SOURCE_VRNG,
        .device_id = 4,
        .vendor_id = VIRTIO_VENDOR_ID,
        .device_features = VIRTIO_RNG_F_VERSION_1,
        .required_features = VIRTIO_RNG_F_VERSION_1,
        .queue_max_sizes = queue_max_sizes,
        .num_queues = ARRAY_SIZE(queue_max_sizes),
        .ops = &virtio_rng_ops,
        .opaque = vrng,
    };

    if (virtio_device_common_init(&vrng->common, &config) < 0) {
        fprintf(stderr, "Failed to initialize virtio-rng common device.\n");
        exit(2);
    }

    if (virtio_actor_init(&vrng->actor, &virtio_rng_actor_ops, vrng,
                          ARRAY_SIZE(queue_max_sizes)) < 0) {
        virtio_device_common_destroy(&vrng->common);
        fprintf(stderr, "Failed to initialize virtio-rng actor.\n");
        exit(2);
    }
    vrng->actor_initialized = true;
}

void virtio_rng_destroy(virtio_rng_state_t *vrng)
{
    if (!vrng)
        return;

    if (vrng->actor_initialized) {
        virtio_actor_stop(&vrng->actor);
        virtio_actor_destroy(&vrng->actor);
        vrng->actor_initialized = false;
    }

    virtio_device_common_destroy(&vrng->common);
}
