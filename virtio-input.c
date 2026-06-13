#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "device.h"
#include "ram_access.h"
#include "riscv.h"
#include "riscv_private.h"
#include "utils.h"
#include "virtio-input-codes.h"
#include "virtio-input-event.h"
#include "virtio-mmio.h"
#include "virtio.h"

/* Threading invariant: guest-facing virtqueue draining is owned by the
 * per-device virtio-input actor. Common VirtIO transport, virtqueue, and IRQ
 * state are owned by virtio-mmio/virtq; actor generation and common generation
 * guards suppress stale reset/stop completions.
 *
 * The SDL/main thread produces host input through the SPSC queue in
 * virtio-input-event.c. The I/O/peripheral polling path only observes the
 * per-device wake gates and notifies input actors; it does not write guest
 * RAM, used rings, or IRQ state for input events. Guest MMIO accesses arrive
 * via virtio_input_read()/virtio_input_write() from mem_load()/mem_store().
 *
 * The only cross-thread touch point is virtio_input_irq_pending(), which reads
 * the common VirtIO ISR bits from the PLIC polling path.
 *
 * No vinput-internal mutex is required as long as SDL writes only host SPSC
 * queues and guest-facing state remains actor/common-owned.
 */

#define BUS_VIRTUAL 0x06

#define VINPUT_DEBUG_PREFIX "[SEMU vinput-log]: "

#define VINPUT_KEYBOARD_NAME "VirtIO Keyboard"
#define VINPUT_MOUSE_NAME "VirtIO Mouse"

#define VINPUT_SERIAL "None"

#define VIRTIO_INPUT_F_VERSION_1 (UINT64_C(1) << 32)

#define VIRTIO_INPUT_QUEUE_NUM_MAX 1024

#define PRIV(x) ((struct vinput_data *) (x)->priv)

enum {
    VIRTIO_INPUT_EVENTQ = 0,
    VIRTIO_INPUT_STATUSQ = 1,
};

enum {
    VIRTIO_INPUT_REG_SELECT = 0x100,
    VIRTIO_INPUT_REG_SUBSEL = 0x101,
    VIRTIO_INPUT_REG_SIZE = 0x102,
};

enum virtio_input_config_select {
    VIRTIO_INPUT_CFG_UNSET = 0x00,
    VIRTIO_INPUT_CFG_ID_NAME = 0x01,
    VIRTIO_INPUT_CFG_ID_SERIAL = 0x02,
    VIRTIO_INPUT_CFG_ID_DEVIDS = 0x03,
    VIRTIO_INPUT_CFG_PROP_BITS = 0x10,
    VIRTIO_INPUT_CFG_EV_BITS = 0x11,
    VIRTIO_INPUT_CFG_ABS_INFO = 0x12,
};

PACKED(struct virtio_input_absinfo {
    uint32_t min;
    uint32_t max;
    uint32_t fuzz;
    uint32_t flat;
    uint32_t res;
});

PACKED(struct virtio_input_devids {
    uint16_t bustype;
    uint16_t vendor;
    uint16_t product;
    uint16_t version;
});

PACKED(struct virtio_input_config {
    uint8_t select;
    uint8_t subsel;
    uint8_t size;
    uint8_t reserved[5];
    union {
        char string[VIRTIO_INPUT_CFG_PAYLOAD_SIZE];
        uint8_t bitmap[VIRTIO_INPUT_CFG_PAYLOAD_SIZE];
        struct virtio_input_absinfo abs;
        struct virtio_input_devids ids;
    } u;
});

PACKED(struct virtio_input_event {
    uint16_t type;
    uint16_t code;
    uint32_t value;
});

struct vinput_data {
    virtio_input_state_t *vinput;
    struct virtio_input_config cfg;
    int type; /* VINPUT_KEYBOARD_ID or VINPUT_MOUSE_ID */
};

static struct vinput_data vinput_dev[VINPUT_DEV_CNT];
static int vinput_dev_cnt;
static const char *vinput_dev_name[VINPUT_DEV_CNT] = {
    VINPUT_KEYBOARD_NAME,
    VINPUT_MOUSE_NAME,
};

static inline void vinput_bitmap_set_bit(uint8_t *map, unsigned long bit)
{
    map[bit / 8] |= (uint8_t) (1U << (bit % 8));
}

/* Return the number of bytes the driver needs to read from the config bitmap,
 * defined by the virtio input spec as "highest set byte index + 1".
 */
static inline unsigned long vinput_bitmap_get_size(const uint8_t *bitmap,
                                                   unsigned long max_bytes)
{
    while (max_bytes > 0 && bitmap[max_bytes - 1] == 0)
        max_bytes--;
    return max_bytes;
}

static inline unsigned virtio_input_status_load(virtio_input_state_t *vinput)
{
    return atomic_load_explicit(&vinput->common.status, memory_order_acquire);
}

static inline void virtio_input_set_fail(virtio_input_state_t *vinput)
{
    unsigned status = virtio_input_status_load(vinput);

    virtio_device_common_set_needs_reset(&vinput->common);
    if (status & VIRTIO_STATUS__DRIVER_OK)
        virtio_irq_trigger(&vinput->common.irq, VIRTIO_INT__CONF_CHANGE);
}

static bool virtio_input_actor_generation_current(virtio_input_state_t *vinput,
                                                  struct virtio_actor *actor,
                                                  uint64_t generation)
{
    return vinput && actor == &vinput->actor &&
           virtio_actor_generation(actor) == generation;
}

static bool virtio_input_queue_ready_for_actor(virtio_input_state_t *vinput,
                                               const struct virtq *queue)
{
    unsigned status;

    if (!vinput || !queue || !queue->ready)
        return false;

    status = virtio_input_status_load(vinput);
    return !(status & VIRTIO_STATUS__DEVICE_NEEDS_RESET) &&
           (status & VIRTIO_STATUS__DRIVER_OK);
}

static bool virtio_input_common_generation_current(virtio_input_state_t *vinput,
                                                   uint64_t generation)
{
    bool current;

    if (!vinput || !vinput->common.initialized)
        return false;

    pthread_mutex_lock(&vinput->common.transport_lock);
    current = vinput->common.generation == generation &&
              !vinput->common.reset_in_progress;
    pthread_mutex_unlock(&vinput->common.transport_lock);
    return current;
}

static bool virtio_input_capture_common_generation(virtio_input_state_t *vinput,
                                                   uint64_t *generation)
{
    unsigned status;
    bool current;

    if (!vinput || !vinput->common.initialized || !generation)
        return false;

    pthread_mutex_lock(&vinput->common.transport_lock);
    status = virtio_input_status_load(vinput);
    current = !vinput->common.reset_in_progress &&
              (status & VIRTIO_STATUS__DRIVER_OK) &&
              !(status & VIRTIO_STATUS__DEVICE_NEEDS_RESET);
    if (current)
        *generation = vinput->common.generation;
    pthread_mutex_unlock(&vinput->common.transport_lock);
    return current;
}

static bool virtio_input_begin_actor_completion(virtio_input_state_t *vinput,
                                                uint64_t actor_generation,
                                                uint64_t common_generation)
{
    bool common_current;

    if (!vinput || !vinput->actor_initialized || !vinput->common.initialized)
        return false;

    pthread_mutex_lock(&vinput->common.transport_lock);
    common_current = vinput->common.generation == common_generation &&
                     !vinput->common.reset_in_progress;
    if (!common_current) {
        pthread_mutex_unlock(&vinput->common.transport_lock);
        return false;
    }

    if (!virtio_actor_begin_completion(&vinput->actor, actor_generation)) {
        pthread_mutex_unlock(&vinput->common.transport_lock);
        return false;
    }
    return true;
}

static void virtio_input_end_actor_completion(virtio_input_state_t *vinput)
{
    if (!vinput || !vinput->actor_initialized)
        return;

    virtio_actor_end_completion(&vinput->actor);
    pthread_mutex_unlock(&vinput->common.transport_lock);
}

static void virtio_input_set_fail_for_actor(virtio_input_state_t *vinput,
                                            uint64_t actor_generation,
                                            uint64_t common_generation)
{
    if (!virtio_input_begin_actor_completion(vinput, actor_generation,
                                             common_generation))
        return;
    virtio_input_set_fail(vinput);
    virtio_input_end_actor_completion(vinput);
}

static inline bool virtio_input_is_config_access(uint32_t addr,
                                                 size_t access_size)
{
    const uint32_t base = VIRTIO_Config << 2;
    const uint32_t end = base + (uint32_t) sizeof(struct virtio_input_config);

    /* [base, end) */
    if (access_size == 0 || addr < base || addr >= end)
        return false;
    return access_size <= end - addr;
}

static int virtio_input_queue_available(virtio_input_state_t *vinput,
                                        const struct virtq *queue,
                                        uint16_t *available)
{
    uint16_t avail_idx;
    uint16_t delta;

    if (!queue || !queue->ready || !available)
        return -EINVAL;

    if (!ram_dma_read(vinput->common.dma, queue->driver_addr + 2, &avail_idx,
                      sizeof(avail_idx)))
        return -EFAULT;

    delta = (uint16_t) (avail_idx - queue->last_avail);
    if (delta > queue->queue_size)
        return -EINVAL;

    *available = delta;
    return 0;
}

static guest_size_t virtio_input_iov_bytes(const struct virtq_iov *iov,
                                           size_t count)
{
    guest_size_t total = 0;

    for (size_t i = 0; i < count; i++)
        total += iov[i].len;
    return total;
}

static bool virtio_input_write_event_to_chain(
    virtio_input_state_t *vinput,
    const struct virtq_chain *chain,
    const struct virtio_input_event *event)
{
    const uint8_t *src = (const uint8_t *) event;
    guest_size_t remaining = sizeof(*event);
    guest_size_t offset = 0;

    if (virtio_input_iov_bytes(chain->writable, chain->writable_count) <
        sizeof(*event))
        return false;

    for (size_t i = 0; i < chain->writable_count && remaining != 0; i++) {
        guest_size_t chunk =
            MIN((guest_size_t) chain->writable[i].len, remaining);

        if (!ram_dma_write(vinput->common.dma, chain->writable[i].addr,
                           src + offset, chunk))
            return false;
        offset += chunk;
        remaining -= chunk;
    }

    return remaining == 0;
}

/* Consume all pending buffers from the status queue and return them to the
 * used ring. The guest driver uses statusq to send EV_LED events (Caps Lock,
 * Num Lock, etc.) and acknowledges each buffer so the queue never stalls.
 * SDL has no portable LED-control API, so LED state is not applied to the host
 * keyboard here.
 */
static int virtio_input_drain_statusq(virtio_input_state_t *vinput,
                                      struct virtio_actor *actor,
                                      uint64_t actor_generation,
                                      uint64_t common_generation)
{
    struct virtq *queue = &vinput->common.queues[VIRTIO_INPUT_STATUSQ];
    struct virtq_iov readable[VIRTIO_INPUT_QUEUE_NUM_MAX];
    struct virtq_iov writable[VIRTIO_INPUT_QUEUE_NUM_MAX];
    bool consumed = false;

    if (!virtio_input_actor_generation_current(vinput, actor, actor_generation))
        return 0;
    if (!virtio_input_queue_ready_for_actor(vinput, queue))
        return 0;

    for (;;) {
        struct virtq_chain chain = {
            .readable = readable,
            .readable_capacity = ARRAY_SIZE(readable),
            .writable = writable,
            .writable_capacity = ARRAY_SIZE(writable),
        };
        uint16_t available;
        int ret;

        if (!virtio_input_actor_generation_current(vinput, actor,
                                                   actor_generation))
            return 0;
        if (!virtio_input_common_generation_current(vinput, common_generation))
            return 0;
        ret = virtio_input_queue_available(vinput, queue, &available);
        if (ret < 0) {
            virtio_input_set_fail_for_actor(vinput, actor_generation,
                                            common_generation);
            return 0;
        }
        if (available == 0)
            break;

        ret = virtq_pop(vinput->common.dma, queue, &chain);
        if (!virtio_input_actor_generation_current(vinput, actor,
                                                   actor_generation))
            return 0;
        if (!virtio_input_common_generation_current(vinput, common_generation))
            return 0;
        if (ret < 0) {
            virtio_input_set_fail_for_actor(vinput, actor_generation,
                                            common_generation);
            return 0;
        }
        if (ret == 0)
            break;

        if (chain.writable_count != 0 || chain.readable_count == 0 ||
            virtio_input_iov_bytes(chain.readable, chain.readable_count) <
                sizeof(struct virtio_input_event)) {
            virtio_input_set_fail_for_actor(vinput, actor_generation,
                                            common_generation);
            return 0;
        }

        if (!virtio_input_begin_actor_completion(vinput, actor_generation,
                                                 common_generation))
            return 0;
        if (virtq_add_used(vinput->common.dma, queue, chain.head, 0) < 0) {
            virtio_input_set_fail(vinput);
            virtio_input_end_actor_completion(vinput);
            return 0;
        }
        virtio_input_end_actor_completion(vinput);
        consumed = true;
    }

    if (consumed && virtio_input_begin_actor_completion(
                        vinput, actor_generation, common_generation)) {
        if (!virtq_interrupt_suppressed(vinput->common.dma, queue))
            virtio_irq_trigger(&vinput->common.irq, VIRTIO_INT__USED_RING);
        virtio_input_end_actor_completion(vinput);
    }
    return 0;
}

static int virtio_input_desc_handler(virtio_input_state_t *vinput,
                                     struct virtio_actor *actor,
                                     uint64_t actor_generation,
                                     uint64_t common_generation,
                                     struct virtio_input_event *input_ev,
                                     uint32_t ev_cnt,
                                     struct virtq *queue,
                                     bool *wrote_events)
{
    struct virtq_iov readable[VIRTIO_INPUT_QUEUE_NUM_MAX];
    struct virtq_iov writable[VIRTIO_INPUT_QUEUE_NUM_MAX];
    uint16_t available;
    int ret;

    *wrote_events = false;
    if (!virtio_input_actor_generation_current(vinput, actor, actor_generation))
        return 0;
    if (!virtio_input_common_generation_current(vinput, common_generation))
        return 0;
    ret = virtio_input_queue_available(vinput, queue, &available);
    if (ret < 0) {
        virtio_input_set_fail_for_actor(vinput, actor_generation,
                                        common_generation);
        return 0;
    }

    /* Preserve the old all-or-drop behavior for grouped input events. */
    if (available < ev_cnt)
        return 0;

    for (uint32_t i = 0; i < ev_cnt; i++) {
        struct virtq_chain chain = {
            .readable = readable,
            .readable_capacity = ARRAY_SIZE(readable),
            .writable = writable,
            .writable_capacity = ARRAY_SIZE(writable),
        };
        ret = virtq_pop(vinput->common.dma, queue, &chain);

        if (!virtio_input_actor_generation_current(vinput, actor,
                                                   actor_generation))
            return 0;
        if (!virtio_input_common_generation_current(vinput, common_generation))
            return 0;
        if (ret < 0) {
            virtio_input_set_fail_for_actor(vinput, actor_generation,
                                            common_generation);
            return 0;
        }
        if (ret == 0)
            return 0;

        if (chain.readable_count != 0 || chain.writable_count == 0 ||
            !virtio_input_write_event_to_chain(vinput, &chain, &input_ev[i])) {
            if (!virtio_input_actor_generation_current(vinput, actor,
                                                       actor_generation) ||
                !virtio_input_common_generation_current(vinput,
                                                        common_generation))
                return 0;
            virtio_input_set_fail_for_actor(vinput, actor_generation,
                                            common_generation);
            return 0;
        }

        if (!virtio_input_actor_generation_current(vinput, actor,
                                                   actor_generation))
            return 0;
        if (!virtio_input_common_generation_current(vinput, common_generation))
            return 0;
        if (!virtio_input_begin_actor_completion(vinput, actor_generation,
                                                 common_generation))
            return 0;
        if (virtq_add_used(vinput->common.dma, queue, chain.head,
                           sizeof(struct virtio_input_event)) < 0) {
            virtio_input_set_fail(vinput);
            virtio_input_end_actor_completion(vinput);
            return 0;
        }
        virtio_input_end_actor_completion(vinput);
        *wrote_events = true;
    }

    return 0;
}

static void virtio_input_update_eventq(int dev_id,
                                       struct virtio_actor *actor,
                                       uint64_t actor_generation,
                                       uint64_t common_generation,
                                       struct virtio_input_event *input_ev,
                                       uint32_t ev_cnt)
{
    virtio_input_state_t *vinput = vinput_dev[dev_id].vinput;
    struct virtq *queue;
    unsigned status;
    bool wrote_events;

    if (!vinput)
        return;

    queue = &vinput->common.queues[VIRTIO_INPUT_EVENTQ];
    status = virtio_input_status_load(vinput);

    if (status & VIRTIO_STATUS__DEVICE_NEEDS_RESET)
        return;

    if (!(status & VIRTIO_STATUS__DRIVER_OK) || !queue->ready)
        return;

    if (virtio_input_desc_handler(vinput, actor, actor_generation,
                                  common_generation, input_ev, ev_cnt, queue,
                                  &wrote_events) < 0)
        return;

#if SEMU_INPUT_DEBUG
    if (!wrote_events)
        fprintf(stderr, VINPUT_DEBUG_PREFIX "drop dev=%d (no guest buffers)\n",
                dev_id);
#endif

    if (wrote_events && virtio_input_begin_actor_completion(
                            vinput, actor_generation, common_generation)) {
        if (!virtq_interrupt_suppressed(vinput->common.dma, queue))
            virtio_irq_trigger(&vinput->common.irq, VIRTIO_INT__USED_RING);
        virtio_input_end_actor_completion(vinput);
    }
}

static void virtio_input_update_key(struct virtio_actor *actor,
                                    uint64_t actor_generation,
                                    uint64_t common_generation,
                                    uint32_t key,
                                    uint32_t ev_value)
{
#if SEMU_INPUT_DEBUG
    fprintf(stderr, VINPUT_DEBUG_PREFIX "key code=%u value=%u\n", key,
            ev_value);
#endif
    /* ev_value follows Linux evdev: 0=release, 1=press, 2=repeat */
    struct virtio_input_event input_ev[] = {
        {.type = SEMU_EV_KEY, .code = key, .value = ev_value},
        {.type = SEMU_EV_SYN, .code = SEMU_SYN_REPORT, .value = 0},
    };

    size_t ev_cnt = ARRAY_SIZE(input_ev);
    virtio_input_update_eventq(VINPUT_KEYBOARD_ID, actor, actor_generation,
                               common_generation, input_ev, ev_cnt);
}

static void virtio_input_update_mouse_button_state(struct virtio_actor *actor,
                                                   uint64_t actor_generation,
                                                   uint64_t common_generation,
                                                   uint32_t button,
                                                   bool pressed)
{
#if SEMU_INPUT_DEBUG
    fprintf(stderr, VINPUT_DEBUG_PREFIX "button code=%u pressed=%u\n", button,
            pressed);
#endif
    struct virtio_input_event input_ev[] = {
        {.type = SEMU_EV_KEY, .code = button, .value = pressed},
        {.type = SEMU_EV_SYN, .code = SEMU_SYN_REPORT, .value = 0},
    };

    size_t ev_cnt = ARRAY_SIZE(input_ev);
    virtio_input_update_eventq(VINPUT_MOUSE_ID, actor, actor_generation,
                               common_generation, input_ev, ev_cnt);
}

static void virtio_input_update_mouse_motion(struct virtio_actor *actor,
                                             uint64_t actor_generation,
                                             uint64_t common_generation,
                                             int32_t dx,
                                             int32_t dy)
{
#if SEMU_INPUT_DEBUG
    fprintf(stderr, VINPUT_DEBUG_PREFIX "motion dx=%d dy=%d\n", dx, dy);
#endif
    struct virtio_input_event input_ev[3];
    uint32_t ev_cnt = 0;

    if (dx)
        input_ev[ev_cnt++] = (struct virtio_input_event) {
            .type = SEMU_EV_REL, .code = SEMU_REL_X, .value = (uint32_t) dx};
    if (dy)
        input_ev[ev_cnt++] = (struct virtio_input_event) {
            .type = SEMU_EV_REL, .code = SEMU_REL_Y, .value = (uint32_t) dy};
    if (!ev_cnt)
        return;

    input_ev[ev_cnt++] = (struct virtio_input_event) {
        .type = SEMU_EV_SYN, .code = SEMU_SYN_REPORT, .value = 0};

    virtio_input_update_eventq(VINPUT_MOUSE_ID, actor, actor_generation,
                               common_generation, input_ev, ev_cnt);
}

static void virtio_input_update_scroll(struct virtio_actor *actor,
                                       uint64_t actor_generation,
                                       uint64_t common_generation,
                                       int32_t dx,
                                       int32_t dy)
{
#if SEMU_INPUT_DEBUG
    fprintf(stderr, VINPUT_DEBUG_PREFIX "scroll dx=%d dy=%d\n", dx, dy);
#endif
    /* Build only the non-zero axis events and always terminate with SYN_REPORT.
     * dx > 0: scroll right, dy > 0: scroll up (matches Linux evdev convention).
     */
    struct virtio_input_event input_ev[3];
    uint32_t ev_cnt = 0;

    if (dx)
        input_ev[ev_cnt++] =
            (struct virtio_input_event) {.type = SEMU_EV_REL,
                                         .code = SEMU_REL_HWHEEL,
                                         .value = (uint32_t) dx};
    if (dy)
        input_ev[ev_cnt++] =
            (struct virtio_input_event) {.type = SEMU_EV_REL,
                                         .code = SEMU_REL_WHEEL,
                                         .value = (uint32_t) dy};
    if (!ev_cnt)
        return;

    input_ev[ev_cnt++] = (struct virtio_input_event) {
        .type = SEMU_EV_SYN, .code = SEMU_SYN_REPORT, .value = 0};

    virtio_input_update_eventq(VINPUT_MOUSE_ID, actor, actor_generation,
                               common_generation, input_ev, ev_cnt);
}

static void virtio_input_drain_host_events_for_actor(
    virtio_input_state_t *vinput,
    struct virtio_actor *actor,
    uint64_t actor_generation,
    uint64_t common_generation)
{
    int dev_id = PRIV(vinput)->type;
    struct virtq *queue = &vinput->common.queues[VIRTIO_INPUT_EVENTQ];

    for (;;) {
        struct vinput_cmd event;

        /* Drain one per-device host queue on its actor so SDL and the I/O
         * polling path never touch guest-visible virtio-input state directly.
         *
         * Cross-device ordering is decided by independent actor scheduling and
         * PLIC arbitration between pending IRQs. The old single-thread path
         * already offered no guest-visible cross-device ordering guarantee
         * beyond interrupt arbitration.
         *
         * If a future change raises interrupts mid-drain, adds host-side
         * timestamps to virtio_input_event, or otherwise starts to rely on
         * sub-tick cross-device ordering, introduce an explicit sequencer.
         */
        if (!virtio_input_queue_ready_for_actor(vinput, queue))
            return;
        while (vinput_pop_cmd(dev_id, &event)) {
            switch (event.type) {
            case VINPUT_CMD_KEYBOARD_KEY:
                if (dev_id == VINPUT_KEYBOARD_ID)
                    virtio_input_update_key(
                        actor, actor_generation, common_generation,
                        event.u.keyboard_key.key, event.u.keyboard_key.value);
                break;
            case VINPUT_CMD_MOUSE_BUTTON:
                if (dev_id == VINPUT_MOUSE_ID)
                    virtio_input_update_mouse_button_state(
                        actor, actor_generation, common_generation,
                        event.u.mouse_button.button,
                        event.u.mouse_button.pressed);
                break;
            case VINPUT_CMD_MOUSE_MOTION:
                if (dev_id == VINPUT_MOUSE_ID)
                    virtio_input_update_mouse_motion(
                        actor, actor_generation, common_generation,
                        event.u.mouse_motion.dx, event.u.mouse_motion.dy);
                break;
            case VINPUT_CMD_MOUSE_WHEEL:
                if (dev_id == VINPUT_MOUSE_ID)
                    virtio_input_update_scroll(
                        actor, actor_generation, common_generation,
                        event.u.mouse_wheel.dx, event.u.mouse_wheel.dy);
                break;
            default:
                break;
            }
        }

        if (vinput_rearm_cmd_wake(dev_id))
            break;
    }
}

void virtio_input_drain_host_events(void)
{
    for (int dev_id = 0; dev_id < VINPUT_DEV_CNT; dev_id++) {
        virtio_input_state_t *vinput = vinput_dev[dev_id].vinput;
        int ret;

        if (!vinput || !vinput_may_have_pending_cmds_for_dev(dev_id))
            continue;

        ret = virtio_actor_notify_queue(&vinput->actor, VIRTIO_INPUT_EVENTQ);
        if (ret == 0 || ret == -EAGAIN)
            continue;
        virtio_input_set_fail(vinput);
    }
}

static void virtio_input_properties(int dev_id)
{
    struct virtio_input_config *cfg = &vinput_dev[dev_id].cfg;
    memset(cfg->u.bitmap, 0, VIRTIO_INPUT_CFG_PAYLOAD_SIZE);

    switch (dev_id) {
    case VINPUT_KEYBOARD_ID:
        cfg->size = 0;
        break;
    case VINPUT_MOUSE_ID:
        /* INPUT_PROP_POINTER marks this as a pointer device. */
        vinput_bitmap_set_bit(cfg->u.bitmap, SEMU_INPUT_PROP_POINTER);
        cfg->size = (uint8_t) vinput_bitmap_get_size(
            cfg->u.bitmap, VIRTIO_INPUT_CFG_PAYLOAD_SIZE);
        break;
    }
}

static void virtio_input_fill_keyboard_ev_bits(int dev_id, uint8_t event)
{
    struct virtio_input_config *cfg = &vinput_dev[dev_id].cfg;
    memset(cfg->u.bitmap, 0, VIRTIO_INPUT_CFG_PAYLOAD_SIZE);

    switch (event) {
    case SEMU_EV_KEY:
        /* Only advertise key codes that key_map[] actually generates. */
        cfg->size = (uint8_t) virtio_input_fill_ev_key_bitmap(
            cfg->u.bitmap, VIRTIO_INPUT_CFG_PAYLOAD_SIZE);
        break;
    case SEMU_EV_LED:
        vinput_bitmap_set_bit(cfg->u.bitmap, SEMU_LED_NUML);
        vinput_bitmap_set_bit(cfg->u.bitmap, SEMU_LED_CAPSL);
        vinput_bitmap_set_bit(cfg->u.bitmap, SEMU_LED_SCROLLL);
        cfg->size = (uint8_t) vinput_bitmap_get_size(
            cfg->u.bitmap, VIRTIO_INPUT_CFG_PAYLOAD_SIZE);
        break;
    case SEMU_EV_REP:
        vinput_bitmap_set_bit(cfg->u.bitmap, SEMU_REP_DELAY);
        vinput_bitmap_set_bit(cfg->u.bitmap, SEMU_REP_PERIOD);
        cfg->size = (uint8_t) vinput_bitmap_get_size(
            cfg->u.bitmap, VIRTIO_INPUT_CFG_PAYLOAD_SIZE);
        break;
    default:
        cfg->size = 0;
    }
}

static void virtio_input_fill_mouse_ev_bits(int dev_id, uint8_t event)
{
    struct virtio_input_config *cfg = &vinput_dev[dev_id].cfg;
    memset(cfg->u.bitmap, 0, VIRTIO_INPUT_CFG_PAYLOAD_SIZE);

    switch (event) {
    case SEMU_EV_KEY:
        vinput_bitmap_set_bit(cfg->u.bitmap, SEMU_BTN_LEFT);
        vinput_bitmap_set_bit(cfg->u.bitmap, SEMU_BTN_RIGHT);
        vinput_bitmap_set_bit(cfg->u.bitmap, SEMU_BTN_MIDDLE);
        cfg->size = (uint8_t) vinput_bitmap_get_size(
            cfg->u.bitmap, VIRTIO_INPUT_CFG_PAYLOAD_SIZE);
        break;
    case SEMU_EV_REL:
        vinput_bitmap_set_bit(cfg->u.bitmap, SEMU_REL_X);
        vinput_bitmap_set_bit(cfg->u.bitmap, SEMU_REL_Y);
        vinput_bitmap_set_bit(cfg->u.bitmap, SEMU_REL_HWHEEL);
        vinput_bitmap_set_bit(cfg->u.bitmap, SEMU_REL_WHEEL);
        cfg->size = (uint8_t) vinput_bitmap_get_size(
            cfg->u.bitmap, VIRTIO_INPUT_CFG_PAYLOAD_SIZE);
        break;
    default:
        cfg->size = 0;
    }
}

static void virtio_input_fill_ev_bits(int dev_id, uint8_t event)
{
    switch (dev_id) {
    case VINPUT_KEYBOARD_ID:
        virtio_input_fill_keyboard_ev_bits(dev_id, event);
        break;
    case VINPUT_MOUSE_ID:
        virtio_input_fill_mouse_ev_bits(dev_id, event);
        break;
    }
}

static void virtio_input_fill_abs_info(int dev_id, uint8_t code)
{
    struct virtio_input_config *cfg = &vinput_dev[dev_id].cfg;
    (void) code;

    /* The current pointing device is a relative mouse, so no ABS axes or
     * ABS_INFO ranges are exposed.
     */
    cfg->size = 0;
}

static void virtio_input_cfg_read(int dev_id)
{
    struct virtio_input_config *cfg = &vinput_dev[dev_id].cfg;
    memset(&cfg->u, 0, sizeof(cfg->u));
    cfg->size = 0;

    switch (cfg->select) {
    case VIRTIO_INPUT_CFG_UNSET:
        return;
    case VIRTIO_INPUT_CFG_ID_NAME:
        strcpy(cfg->u.string, vinput_dev_name[dev_id]);
        cfg->size = strlen(vinput_dev_name[dev_id]);
        return;
    case VIRTIO_INPUT_CFG_ID_SERIAL:
        strcpy(cfg->u.string, VINPUT_SERIAL);
        cfg->size = strlen(VINPUT_SERIAL);
        return;
    case VIRTIO_INPUT_CFG_ID_DEVIDS:
        cfg->u.ids.bustype = BUS_VIRTUAL;
        cfg->u.ids.vendor = 0;
        cfg->u.ids.product = 0;
        cfg->u.ids.version = 1;
        cfg->size = sizeof(struct virtio_input_devids);
        return;
    case VIRTIO_INPUT_CFG_PROP_BITS:
        virtio_input_properties(dev_id);
        return;
    case VIRTIO_INPUT_CFG_EV_BITS:
        virtio_input_fill_ev_bits(dev_id, cfg->subsel);
        return;
    case VIRTIO_INPUT_CFG_ABS_INFO:
        virtio_input_fill_abs_info(dev_id, cfg->subsel);
        return;
    default:
        return;
    }
}

static bool virtio_input_config_range_valid(uint32_t offset, uint32_t size)
{
    return size != 0 && offset < sizeof(struct virtio_input_config) &&
           size <= sizeof(struct virtio_input_config) - offset;
}

static bool virtio_input_range_overlaps(uint32_t offset,
                                        uint32_t size,
                                        uint32_t field_offset,
                                        uint32_t field_size)
{
    return offset < field_offset + field_size && field_offset < offset + size;
}

static uint32_t virtio_input_read_config(void *opaque,
                                         uint32_t offset,
                                         uint32_t size)
{
    virtio_input_state_t *vinput = opaque;
    struct virtio_input_config *cfg = &PRIV(vinput)->cfg;
    uint32_t value = 0;

    if (!virtio_input_config_range_valid(offset, size))
        return 0;

    if (virtio_input_range_overlaps(offset, size,
                                    offsetof(struct virtio_input_config, size),
                                    sizeof(cfg->size)))
        virtio_input_cfg_read(PRIV(vinput)->type);

    memcpy(&value, (uint8_t *) cfg + offset, size);
    return value;
}

static void virtio_input_write_config(void *opaque,
                                      uint32_t offset,
                                      uint32_t size,
                                      uint32_t value)
{
    virtio_input_state_t *vinput = opaque;
    uint8_t *dst = (uint8_t *) &PRIV(vinput)->cfg;
    uint8_t *src = (uint8_t *) &value;

    if (!virtio_input_config_range_valid(offset, size))
        return;

    for (uint32_t i = 0; i < size; i++)
        dst[offset + i] = src[i];
}

static bool virtio_input_load_width_bytes(uint8_t width, size_t *access_size)
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

static bool virtio_input_store_width_bytes(uint8_t width, size_t *access_size)
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

static bool virtio_input_config_write_allowed(uint32_t addr, size_t size)
{
    uint32_t offset = addr - (VIRTIO_Config << 2);

    return virtio_input_config_range_valid(offset, (uint32_t) size) &&
           offset < offsetof(struct virtio_input_config, subsel) + 1 &&
           size <= offsetof(struct virtio_input_config, subsel) + 1 - offset;
}

void virtio_input_read(hart_t *vm,
                       virtio_input_state_t *vinput,
                       uint32_t addr,
                       uint8_t width,
                       uint32_t *value)
{
    size_t access_size = 0;
    bool is_cfg;
    int ret;

    if (!virtio_input_load_width_bytes(width, &access_size)) {
        vm_set_exception(vm, RV_EXC_ILLEGAL_INSN, 0);
        return;
    }

    is_cfg = virtio_input_is_config_access(addr, access_size);
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

    ret = virtio_mmio_read(&vinput->common, addr, (uint8_t) access_size, value);
    if (ret < 0)
        vm_set_exception(vm, RV_EXC_LOAD_FAULT, vm->exc_val);
}

void virtio_input_write(hart_t *vm,
                        virtio_input_state_t *vinput,
                        uint32_t addr,
                        uint8_t width,
                        uint32_t value)
{
    size_t access_size = 0;
    bool is_cfg;
    int ret;

    if (!virtio_input_store_width_bytes(width, &access_size)) {
        vm_set_exception(vm, RV_EXC_ILLEGAL_INSN, 0);
        return;
    }

    is_cfg = virtio_input_is_config_access(addr, access_size);
    if (addr >= (VIRTIO_Config << 2) && !is_cfg) {
        vm_set_exception(vm, RV_EXC_STORE_FAULT, vm->exc_val);
        return;
    }

    if (!is_cfg) {
        if (access_size != 4 || (addr & 0x3)) {
            vm_set_exception(vm, RV_EXC_STORE_MISALIGN, vm->exc_val);
            return;
        }
    } else {
        if (addr & (access_size - 1)) {
            vm_set_exception(vm, RV_EXC_STORE_MISALIGN, vm->exc_val);
            return;
        }
        if (!virtio_input_config_write_allowed(addr, access_size)) {
            vm_set_exception(vm, RV_EXC_STORE_FAULT, vm->exc_val);
            return;
        }
    }

    ret =
        virtio_mmio_write(&vinput->common, addr, (uint8_t) access_size, value);
    if (ret < 0)
        vm_set_exception(vm, RV_EXC_STORE_FAULT, vm->exc_val);
}

bool virtio_input_irq_pending(virtio_input_state_t *vinput)
{
    return virtio_irq_read_status(&vinput->common.irq) != 0;
}

static int virtio_input_actor_drain_queue(void *opaque,
                                          struct virtio_actor *actor,
                                          uint16_t queue_index,
                                          uint64_t generation)
{
    virtio_input_state_t *vinput = opaque;
    uint64_t common_generation = 0;

    if (!vinput || (queue_index != VIRTIO_INPUT_EVENTQ &&
                    queue_index != VIRTIO_INPUT_STATUSQ)) {
        if (vinput)
            virtio_input_set_fail(vinput);
        return 0;
    }

    if (!virtio_input_actor_generation_current(vinput, actor, generation))
        return 0;
    if (!virtio_input_capture_common_generation(vinput, &common_generation))
        return 0;

    if (queue_index == VIRTIO_INPUT_EVENTQ) {
        virtio_input_drain_host_events_for_actor(vinput, actor, generation,
                                                 common_generation);
        return 0;
    }

    return virtio_input_drain_statusq(vinput, actor, generation,
                                      common_generation);
}

static bool virtio_input_actor_queue_has_work(void *opaque,
                                              struct virtio_actor *actor,
                                              uint16_t queue_index,
                                              uint64_t generation)
{
    virtio_input_state_t *vinput = opaque;
    uint16_t available = 0;

    if (!vinput || (queue_index != VIRTIO_INPUT_EVENTQ &&
                    queue_index != VIRTIO_INPUT_STATUSQ))
        return false;
    if (!virtio_input_actor_generation_current(vinput, actor, generation))
        return false;

    if (queue_index == VIRTIO_INPUT_EVENTQ) {
        if (!virtio_input_queue_ready_for_actor(
                vinput, &vinput->common.queues[VIRTIO_INPUT_EVENTQ]))
            return false;
        return vinput_may_have_pending_cmds_for_dev(PRIV(vinput)->type);
    }

    if (!virtio_input_queue_ready_for_actor(
            vinput, &vinput->common.queues[VIRTIO_INPUT_STATUSQ]))
        return false;
    if (virtio_input_queue_available(
            vinput, &vinput->common.queues[VIRTIO_INPUT_STATUSQ], &available) <
        0)
        return false;
    return available != 0;
}

static void virtio_input_actor_failed(void *opaque,
                                      struct virtio_actor *actor UNUSED)
{
    virtio_input_state_t *vinput = opaque;

    if (vinput)
        virtio_input_set_fail(vinput);
}

static const struct virtio_actor_ops virtio_input_actor_ops = {
    .drain_queue = virtio_input_actor_drain_queue,
    .queue_has_work = virtio_input_actor_queue_has_work,
    .on_failed = virtio_input_actor_failed,
};

static int virtio_input_activate(void *opaque,
                                 const struct virtio_activation_context *ctx)
{
    virtio_input_state_t *vinput = opaque;
    int ret;

    (void) ctx;

    if (!vinput || !vinput->actor_initialized)
        return -EINVAL;

    ret = virtio_actor_start(&vinput->actor);
    if (ret < 0 && ret != -EALREADY)
        return ret;

    ret = virtio_actor_enter_configuring(&vinput->actor);
    if (ret < 0)
        return ret;
    return virtio_actor_activate(&vinput->actor);
}

static int virtio_input_prepare_reset(void *opaque,
                                      uint64_t old_generation,
                                      uint64_t new_generation)
{
    virtio_input_state_t *vinput = opaque;

    (void) old_generation;
    (void) new_generation;

    if (!vinput || !vinput->actor_initialized)
        return 0;
    return virtio_actor_reset(&vinput->actor);
}

static int virtio_input_reset(void *opaque,
                              uint64_t old_generation,
                              uint64_t new_generation)
{
    virtio_input_state_t *vinput = opaque;
    (void) old_generation;
    (void) new_generation;

    vinput_reset_host_events(PRIV(vinput)->type);
    return 0;
}

static int virtio_input_notify_queue(void *opaque,
                                     uint16_t queue_index,
                                     uint64_t generation)
{
    virtio_input_state_t *vinput = opaque;
    int ret;
    (void) generation;

    if (!vinput || (queue_index != VIRTIO_INPUT_EVENTQ &&
                    queue_index != VIRTIO_INPUT_STATUSQ)) {
        if (vinput)
            virtio_input_set_fail(vinput);
        return -EINVAL;
    }

    ret = virtio_actor_notify_queue(&vinput->actor, queue_index);
    if (ret == 0 || ret == -EAGAIN)
        return 0;

    virtio_input_set_fail(vinput);
    return ret;
}

static const struct virtio_device_ops virtio_input_ops = {
    .activate = virtio_input_activate,
    .prepare_reset = virtio_input_prepare_reset,
    .reset = virtio_input_reset,
    .notify_queue = virtio_input_notify_queue,
    .read_config = virtio_input_read_config,
    .write_config = virtio_input_write_config,
};

void virtio_input_init(virtio_input_state_t *vinput,
                       emu_state_t *emu,
                       enum semu_irq_source irq_source)
{
    static const uint16_t queue_max_sizes[] = {
        [VIRTIO_INPUT_EVENTQ] = VIRTIO_INPUT_QUEUE_NUM_MAX,
        [VIRTIO_INPUT_STATUSQ] = VIRTIO_INPUT_QUEUE_NUM_MAX,
    };
    int dev_id;
    struct virtio_device_common_config config;

    if (vinput_dev_cnt >= VINPUT_DEV_CNT) {
        fprintf(stderr,
                "Exceeded the number of virtio-input devices that can be "
                "allocated.\n");
        exit(2);
    }

    dev_id = vinput_dev_cnt;
    memset(vinput, 0, sizeof(*vinput));
    vinput->ram = emu->ram;
    vinput->priv = &vinput_dev[dev_id];
    PRIV(vinput)->type = dev_id;
    PRIV(vinput)->vinput = vinput;

    config = (struct virtio_device_common_config) {
        .emu = emu,
        .dma = &emu->ram_dma,
        .irq_source = irq_source,
        .device_id = 18,
        .vendor_id = VIRTIO_VENDOR_ID,
        .device_features = VIRTIO_INPUT_F_VERSION_1,
        .required_features = VIRTIO_INPUT_F_VERSION_1,
        .queue_max_sizes = queue_max_sizes,
        .num_queues = ARRAY_SIZE(queue_max_sizes),
        .ops = &virtio_input_ops,
        .opaque = vinput,
    };

    if (virtio_device_common_init(&vinput->common, &config) < 0) {
        fprintf(stderr, "Failed to initialize virtio-input common device.\n");
        exit(2);
    }

    if (virtio_actor_init(&vinput->actor, &virtio_input_actor_ops, vinput,
                          ARRAY_SIZE(queue_max_sizes)) < 0) {
        virtio_device_common_destroy(&vinput->common);
        fprintf(stderr, "Failed to initialize virtio-input actor.\n");
        exit(2);
    }
    vinput->actor_initialized = true;
    vinput_dev_cnt++;
}

void virtio_input_destroy(virtio_input_state_t *vinput)
{
    if (!vinput)
        return;

    if (vinput->actor_initialized) {
        virtio_actor_stop(&vinput->actor);
        virtio_actor_destroy(&vinput->actor);
        vinput->actor_initialized = false;
    }

    if (vinput->priv) {
        struct vinput_data *data = PRIV(vinput);

        if (data->vinput == vinput)
            data->vinput = NULL;
        data->cfg = (struct virtio_input_config) {0};
    }

    virtio_device_common_destroy(&vinput->common);

    if (vinput_dev[VINPUT_KEYBOARD_ID].vinput == NULL &&
        vinput_dev[VINPUT_MOUSE_ID].vinput == NULL)
        vinput_dev_cnt = 0;
}
