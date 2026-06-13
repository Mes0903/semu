#include "virtio-mmio.h"

#include "virtio.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define VIRTIO_MMIO_REG(reg) ((uint32_t) VIRTIO_##reg << 2)
#define VIRTIO_QUEUE_INVALID UINT32_MAX
#define VIRTIO_STATUS_DRIVER_MASK                            \
    (VIRTIO_STATUS__ACKNOWLEDGE | VIRTIO_STATUS__DRIVER |    \
     VIRTIO_STATUS__FEATURES_OK | VIRTIO_STATUS__DRIVER_OK | \
     VIRTIO_STATUS__FAILED)

static bool virtio_mmio_common_width_ok(uint8_t width)
{
    return width == sizeof(uint32_t);
}

static bool virtio_mmio_config_width_ok(uint8_t width)
{
    return width == 1 || width == 2 || width == 4;
}

static uint32_t virtio_mmio_feature_half(uint64_t features, uint32_t sel)
{
    if (sel == 0)
        return (uint32_t) features;
    if (sel == 1)
        return (uint32_t) (features >> 32);
    return 0;
}

static void virtio_mmio_store_feature_half(uint64_t *features,
                                           uint32_t sel,
                                           uint32_t value)
{
    if (sel == 0) {
        *features &= UINT64_C(0xffffffff00000000);
        *features |= value;
    } else if (sel == 1) {
        *features &= UINT64_C(0x00000000ffffffff);
        *features |= (uint64_t) value << 32;
    }
}

static bool virtio_mmio_selected_queue_valid(
    const struct virtio_device_common *common)
{
    return common && common->queue_sel < common->num_queues;
}

static struct virtio_queue_common *virtio_mmio_selected_queue_cfg(
    struct virtio_device_common *common)
{
    if (!virtio_mmio_selected_queue_valid(common))
        return NULL;
    return &common->queue_cfgs[common->queue_sel];
}

static void virtio_mmio_queue_cfg_reset(struct virtio_queue_common *cfg)
{
    uint16_t max_size = cfg->max_size;

    memset(cfg, 0, sizeof(*cfg));
    cfg->max_size = max_size;
}

static bool virtio_mmio_all_queues_ready(
    const struct virtio_device_common *common)
{
    for (uint16_t i = 0; i < common->num_queues; i++) {
        if (common->queue_cfgs[i].max_size != 0 && !common->queues[i].ready)
            return false;
    }
    return true;
}

struct virtio_activation_request {
    const struct virtio_device_ops *ops;
    void *opaque;
    struct virtio_activation_context ctx;
    bool pending;
};

static int virtio_mmio_validate_status_order(unsigned old_status,
                                             uint32_t value)
{
    unsigned combined = old_status | value;
    const unsigned acknowledge_driver =
        VIRTIO_STATUS__ACKNOWLEDGE | VIRTIO_STATUS__DRIVER;
    const unsigned ready_for_driver_ok =
        acknowledge_driver | VIRTIO_STATUS__FEATURES_OK;

    if (old_status & VIRTIO_STATUS__DEVICE_NEEDS_RESET)
        return -EPERM;
    if (value & ~VIRTIO_STATUS_DRIVER_MASK)
        return -EINVAL;
    if ((value & VIRTIO_STATUS__DRIVER) &&
        !(combined & VIRTIO_STATUS__ACKNOWLEDGE))
        return -EINVAL;
    if ((value & VIRTIO_STATUS__FEATURES_OK) &&
        (combined & acknowledge_driver) != acknowledge_driver)
        return -EINVAL;
    if ((value & VIRTIO_STATUS__DRIVER_OK) &&
        (combined & ready_for_driver_ok) != ready_for_driver_ok)
        return -EINVAL;
    return 0;
}

static void virtio_mmio_prepare_activation_request_locked(
    struct virtio_device_common *common,
    struct virtio_activation_request *request)
{
    if (!common->ops || !common->ops->activate) {
        common->activated = true;
        return;
    }

    request->ops = common->ops;
    request->opaque = common->opaque;
    request->ctx.emu = common->emu;
    request->ctx.common = common;
    request->ctx.queues = common->queues;
    request->ctx.num_queues = common->num_queues;
    request->ctx.irq = &common->irq;
    request->ctx.generation = common->generation;
    request->pending = true;
}

static int virtio_mmio_complete_activation(
    struct virtio_device_common *common,
    const struct virtio_activation_request *request)
{
    unsigned status;
    bool still_current;
    int ret;

    if (!request->pending)
        return 0;

    pthread_mutex_lock(&common->backend_lock);
    pthread_mutex_lock(&common->transport_lock);
    status = atomic_load_explicit(&common->status, memory_order_acquire);
    still_current = common->generation == request->ctx.generation &&
                    (status & VIRTIO_STATUS__DRIVER_OK);
    pthread_mutex_unlock(&common->transport_lock);

    if (!still_current) {
        pthread_mutex_unlock(&common->backend_lock);
        return -ECANCELED;
    }

    ret = request->ops->activate(request->opaque, &request->ctx);

    pthread_mutex_lock(&common->transport_lock);
    status = atomic_load_explicit(&common->status, memory_order_acquire);
    if (ret < 0) {
        atomic_fetch_or_explicit(&common->status,
                                 VIRTIO_STATUS__DEVICE_NEEDS_RESET,
                                 memory_order_release);
        common->activated = false;
    } else if (common->generation == request->ctx.generation &&
               (status & VIRTIO_STATUS__DRIVER_OK)) {
        common->activated = true;
    }
    pthread_mutex_unlock(&common->transport_lock);
    pthread_mutex_unlock(&common->backend_lock);
    return ret;
}

static int virtio_mmio_complete_notify(struct virtio_device_common *common,
                                       const struct virtio_device_ops *ops,
                                       void *opaque,
                                       uint16_t queue_index,
                                       uint64_t generation)
{
    bool still_current;

    pthread_mutex_lock(&common->backend_lock);
    pthread_mutex_lock(&common->transport_lock);
    still_current = common->generation == generation &&
                    queue_index < common->num_queues &&
                    common->queues[queue_index].ready;
    pthread_mutex_unlock(&common->transport_lock);

    if (!still_current) {
        pthread_mutex_unlock(&common->backend_lock);
        return -ECANCELED;
    }

    ops->notify_queue(opaque, queue_index, generation);
    pthread_mutex_unlock(&common->backend_lock);
    return 0;
}

static int virtio_mmio_set_status_locked(
    struct virtio_device_common *common,
    uint32_t value,
    struct virtio_activation_request *activation_request)
{
    unsigned old_status =
        atomic_load_explicit(&common->status, memory_order_acquire);
    unsigned new_status;
    bool driver_ok_edge;
    int ret;

    ret = virtio_mmio_validate_status_order(old_status, value);
    if (ret < 0)
        return ret;

    if (value & VIRTIO_STATUS__FEATURES_OK) {
        if ((common->driver_features & ~common->device_features) != 0 ||
            (common->required_features & ~common->driver_features) != 0) {
            atomic_store_explicit(&common->status,
                                  old_status & ~VIRTIO_STATUS__FEATURES_OK,
                                  memory_order_release);
            return -EINVAL;
        }
    }

    driver_ok_edge = (value & VIRTIO_STATUS__DRIVER_OK) &&
                     !(old_status & VIRTIO_STATUS__DRIVER_OK);
    if (driver_ok_edge && !virtio_mmio_all_queues_ready(common))
        return -EINVAL;

    new_status = old_status | (value & VIRTIO_STATUS_DRIVER_MASK);
    atomic_store_explicit(&common->status, new_status, memory_order_release);

    if (driver_ok_edge && !common->activated)
        virtio_mmio_prepare_activation_request_locked(common,
                                                      activation_request);

    return 0;
}

static int virtio_mmio_prepare_queue(struct virtio_device_common *common,
                                     uint32_t value)
{
    struct virtio_queue_common *cfg;
    struct virtq *vq;
    int ret;

    cfg = virtio_mmio_selected_queue_cfg(common);
    if (!cfg)
        return -EINVAL;
    vq = &common->queues[common->queue_sel];

    if (value == 0)
        return vq->ready ? -EPERM : 0;
    if (value != 1)
        return -EINVAL;
    if (vq->ready)
        return 0;
    if (cfg->queue_num == 0 || cfg->queue_num > cfg->max_size)
        return -EINVAL;

    ret = virtq_configure(vq, common->dma, cfg->queue_num, cfg->desc_addr,
                          cfg->driver_addr, cfg->device_addr,
                          common->driver_features);
    if (ret < 0)
        return ret;
    return 0;
}

static int virtio_mmio_write_queue_addr_low(guest_paddr_t *addr, uint32_t value)
{
    *addr = (*addr & UINT64_C(0xffffffff00000000)) | value;
    return 0;
}

static int virtio_mmio_write_queue_addr_high(guest_paddr_t *addr,
                                             uint32_t value)
{
    *addr =
        (*addr & UINT64_C(0x00000000ffffffff)) | ((guest_paddr_t) value << 32);
    return 0;
}

int virtio_device_common_init(struct virtio_device_common *common,
                              const struct virtio_device_common_config *config)
{
    if (!common || !config || !config->queue_max_sizes ||
        config->num_queues == 0)
        return -EINVAL;

    memset(common, 0, sizeof(*common));
    common->queues = calloc(config->num_queues, sizeof(*common->queues));
    common->queue_cfgs =
        calloc(config->num_queues, sizeof(*common->queue_cfgs));
    if (!common->queues || !common->queue_cfgs) {
        free(common->queues);
        free(common->queue_cfgs);
        memset(common, 0, sizeof(*common));
        return -ENOMEM;
    }

    if (pthread_mutex_init(&common->transport_lock, NULL) != 0) {
        free(common->queues);
        free(common->queue_cfgs);
        memset(common, 0, sizeof(*common));
        return -ENOMEM;
    }
    if (pthread_mutex_init(&common->backend_lock, NULL) != 0) {
        pthread_mutex_destroy(&common->transport_lock);
        free(common->queues);
        free(common->queue_cfgs);
        memset(common, 0, sizeof(*common));
        return -ENOMEM;
    }

    common->device_id = config->device_id;
    common->vendor_id = config->vendor_id;
    common->device_features = config->device_features;
    common->required_features = config->required_features;
    common->num_queues = config->num_queues;
    common->emu = config->emu;
    common->dma = config->dma;
    common->ops = config->ops;
    common->opaque = config->opaque;
    atomic_init(&common->status, 0);
    common->generation = 1;

    for (uint16_t i = 0; i < config->num_queues; i++) {
        virtq_init(&common->queues[i]);
        common->queue_cfgs[i].max_size = config->queue_max_sizes[i];
    }

    if (config->irq_source != SEMU_IRQ_SOURCE_COUNT) {
        if (!config->emu || virtio_irq_init(&common->irq, config->emu,
                                            config->irq_source) != 0) {
            pthread_mutex_destroy(&common->backend_lock);
            pthread_mutex_destroy(&common->transport_lock);
            free(common->queues);
            free(common->queue_cfgs);
            memset(common, 0, sizeof(*common));
            return -EINVAL;
        }
        common->irq_initialized = true;
    }

    common->initialized = true;
    return 0;
}

void virtio_device_common_destroy(struct virtio_device_common *common)
{
    if (!common || !common->initialized)
        return;

    if (common->irq_initialized)
        virtio_irq_destroy(&common->irq);
    pthread_mutex_destroy(&common->backend_lock);
    pthread_mutex_destroy(&common->transport_lock);
    free(common->queues);
    free(common->queue_cfgs);
    memset(common, 0, sizeof(*common));
}

int virtio_device_common_reset(struct virtio_device_common *common)
{
    uint64_t old_generation;
    uint64_t new_generation;
    const struct virtio_device_ops *ops;
    void *opaque;
    int ret = 0;

    if (!common || !common->initialized)
        return -EINVAL;

    pthread_mutex_lock(&common->backend_lock);
    pthread_mutex_lock(&common->transport_lock);
    old_generation = common->generation;
    new_generation = old_generation + 1;
    common->generation = new_generation;
    common->driver_features = 0;
    common->device_features_sel = 0;
    common->driver_features_sel = 0;
    common->queue_sel = 0;
    common->activated = false;
    atomic_store_explicit(&common->status, 0, memory_order_release);

    for (uint16_t i = 0; i < common->num_queues; i++) {
        virtq_init(&common->queues[i]);
        virtio_mmio_queue_cfg_reset(&common->queue_cfgs[i]);
    }
    if (common->irq_initialized)
        virtio_irq_ack(&common->irq, UINT32_MAX);
    ops = common->ops;
    opaque = common->opaque;
    pthread_mutex_unlock(&common->transport_lock);

    if (ops && ops->reset)
        ret = ops->reset(opaque, old_generation, new_generation);
    pthread_mutex_unlock(&common->backend_lock);
    return ret;
}

void virtio_device_common_set_needs_reset(struct virtio_device_common *common)
{
    if (!common || !common->initialized)
        return;
    atomic_fetch_or_explicit(&common->status, VIRTIO_STATUS__DEVICE_NEEDS_RESET,
                             memory_order_release);
}

int virtio_mmio_read(struct virtio_device_common *common,
                     uint32_t byte_offset,
                     uint8_t width,
                     uint32_t *value)
{
    struct virtio_queue_common *cfg;
    uint32_t result = 0;
    int ret = 0;

    if (!common || !common->initialized || !value)
        return -EINVAL;
    if (byte_offset >= VIRTIO_MMIO_REG(Config)) {
        uint32_t config_offset = byte_offset - VIRTIO_MMIO_REG(Config);
        const struct virtio_device_ops *ops;
        void *opaque;

        if (!virtio_mmio_config_width_ok(width))
            return -EINVAL;
        pthread_mutex_lock(&common->transport_lock);
        ops = common->ops;
        opaque = common->opaque;
        pthread_mutex_unlock(&common->transport_lock);
        if (ops && ops->read_config)
            result = ops->read_config(opaque, config_offset, width);
        *value = result;
        return 0;
    }

    if (!virtio_mmio_common_width_ok(width))
        return -EINVAL;

    pthread_mutex_lock(&common->transport_lock);
    cfg = virtio_mmio_selected_queue_cfg(common);

    switch (byte_offset) {
    case VIRTIO_MMIO_REG(MagicValue):
        result = VIRTIO_MMIO_MAGIC_VALUE;
        break;
    case VIRTIO_MMIO_REG(Version):
        result = VIRTIO_MMIO_VERSION_VALUE;
        break;
    case VIRTIO_MMIO_REG(DeviceID):
        result = common->device_id;
        break;
    case VIRTIO_MMIO_REG(VendorID):
        result = common->vendor_id;
        break;
    case VIRTIO_MMIO_REG(DeviceFeatures):
        result = virtio_mmio_feature_half(common->device_features,
                                          common->device_features_sel);
        break;
    case VIRTIO_MMIO_REG(DeviceFeaturesSel):
        result = common->device_features_sel;
        break;
    case VIRTIO_MMIO_REG(DriverFeatures):
        result = virtio_mmio_feature_half(common->driver_features,
                                          common->driver_features_sel);
        break;
    case VIRTIO_MMIO_REG(DriverFeaturesSel):
        result = common->driver_features_sel;
        break;
    case VIRTIO_MMIO_REG(QueueSel):
        result = common->queue_sel;
        break;
    case VIRTIO_MMIO_REG(QueueNumMax):
        result = cfg ? cfg->max_size : 0;
        break;
    case VIRTIO_MMIO_REG(QueueNum):
        result = cfg ? cfg->queue_num : 0;
        break;
    case VIRTIO_MMIO_REG(QueueReady):
        result = cfg ? common->queues[common->queue_sel].ready : 0;
        break;
    case VIRTIO_MMIO_REG(InterruptStatus):
        result = virtio_irq_read_status(&common->irq);
        break;
    case VIRTIO_MMIO_REG(Status):
        result = atomic_load_explicit(&common->status, memory_order_acquire);
        break;
    case VIRTIO_MMIO_REG(QueueDescLow):
        result = cfg ? (uint32_t) cfg->desc_addr : 0;
        break;
    case VIRTIO_MMIO_REG(QueueDescHigh):
        result = cfg ? (uint32_t) (cfg->desc_addr >> 32) : 0;
        break;
    case VIRTIO_MMIO_REG(QueueDriverLow):
        result = cfg ? (uint32_t) cfg->driver_addr : 0;
        break;
    case VIRTIO_MMIO_REG(QueueDriverHigh):
        result = cfg ? (uint32_t) (cfg->driver_addr >> 32) : 0;
        break;
    case VIRTIO_MMIO_REG(QueueDeviceLow):
        result = cfg ? (uint32_t) cfg->device_addr : 0;
        break;
    case VIRTIO_MMIO_REG(QueueDeviceHigh):
        result = cfg ? (uint32_t) (cfg->device_addr >> 32) : 0;
        break;
    case VIRTIO_MMIO_REG(ConfigGeneration):
        result = common->config_generation;
        break;
    case VIRTIO_MMIO_REG(SHMSel):
    case VIRTIO_MMIO_REG(SHMLenLow):
    case VIRTIO_MMIO_REG(SHMLenHigh):
    case VIRTIO_MMIO_REG(SHMBaseLow):
    case VIRTIO_MMIO_REG(SHMBaseHigh):
    case VIRTIO_MMIO_REG(QueueReset):
        result = 0;
        break;
    default:
        ret = -EINVAL;
        break;
    }

    pthread_mutex_unlock(&common->transport_lock);
    if (ret == 0)
        *value = result;
    return ret;
}

int virtio_mmio_write(struct virtio_device_common *common,
                      uint32_t byte_offset,
                      uint8_t width,
                      uint32_t value)
{
    struct virtio_queue_common *cfg;
    struct virtio_activation_request activation_request = {0};
    const struct virtio_device_ops *notify_ops = NULL;
    void *notify_opaque = NULL;
    uint16_t notify_queue = 0;
    uint64_t notify_generation = 0;
    bool notify_after_unlock = false;
    int ret = 0;

    if (!common || !common->initialized)
        return -EINVAL;
    if (byte_offset >= VIRTIO_MMIO_REG(Config)) {
        uint32_t config_offset = byte_offset - VIRTIO_MMIO_REG(Config);
        const struct virtio_device_ops *ops;
        void *opaque;

        if (!virtio_mmio_config_width_ok(width))
            return -EINVAL;
        pthread_mutex_lock(&common->transport_lock);
        ops = common->ops;
        opaque = common->opaque;
        pthread_mutex_unlock(&common->transport_lock);
        if (ops && ops->write_config)
            ops->write_config(opaque, config_offset, width, value);
        return 0;
    }

    if (!virtio_mmio_common_width_ok(width))
        return -EINVAL;
    if (byte_offset == VIRTIO_MMIO_REG(Status) && value == 0)
        return virtio_device_common_reset(common);

    pthread_mutex_lock(&common->transport_lock);
    cfg = virtio_mmio_selected_queue_cfg(common);

    switch (byte_offset) {
    case VIRTIO_MMIO_REG(DeviceFeaturesSel):
        common->device_features_sel = value;
        break;
    case VIRTIO_MMIO_REG(DriverFeatures):
        if (atomic_load_explicit(&common->status, memory_order_acquire) &
            VIRTIO_STATUS__FEATURES_OK) {
            ret = -EPERM;
            break;
        }
        virtio_mmio_store_feature_half(&common->driver_features,
                                       common->driver_features_sel, value);
        break;
    case VIRTIO_MMIO_REG(DriverFeaturesSel):
        if (atomic_load_explicit(&common->status, memory_order_acquire) &
            VIRTIO_STATUS__FEATURES_OK) {
            ret = -EPERM;
            break;
        }
        common->driver_features_sel = value;
        break;
    case VIRTIO_MMIO_REG(QueueSel):
        common->queue_sel =
            value < common->num_queues ? value : VIRTIO_QUEUE_INVALID;
        break;
    case VIRTIO_MMIO_REG(QueueNum):
        if (!cfg) {
            ret = -EINVAL;
            break;
        }
        if (common->queues[common->queue_sel].ready) {
            ret = -EPERM;
            break;
        }
        if (value == 0 || value > cfg->max_size || value > UINT16_MAX) {
            ret = -EINVAL;
            break;
        }
        cfg->queue_num = (uint16_t) value;
        break;
    case VIRTIO_MMIO_REG(QueueReady):
        ret = virtio_mmio_prepare_queue(common, value);
        break;
    case VIRTIO_MMIO_REG(QueueNotify):
        if (value >= common->num_queues || !common->queues[value].ready) {
            ret = -EINVAL;
            break;
        }
        if (common->ops && common->ops->notify_queue) {
            notify_ops = common->ops;
            notify_opaque = common->opaque;
            notify_queue = (uint16_t) value;
            notify_generation = common->generation;
            notify_after_unlock = true;
        }
        break;
    case VIRTIO_MMIO_REG(InterruptACK):
        virtio_irq_ack(&common->irq, value);
        break;
    case VIRTIO_MMIO_REG(Status):
        ret = virtio_mmio_set_status_locked(common, value, &activation_request);
        break;
    case VIRTIO_MMIO_REG(QueueDescLow):
    case VIRTIO_MMIO_REG(QueueDescHigh):
    case VIRTIO_MMIO_REG(QueueDriverLow):
    case VIRTIO_MMIO_REG(QueueDriverHigh):
    case VIRTIO_MMIO_REG(QueueDeviceLow):
    case VIRTIO_MMIO_REG(QueueDeviceHigh):
        if (!cfg) {
            ret = -EINVAL;
            break;
        }
        if (common->queues[common->queue_sel].ready) {
            ret = -EPERM;
            break;
        }
        switch (byte_offset) {
        case VIRTIO_MMIO_REG(QueueDescLow):
            ret = virtio_mmio_write_queue_addr_low(&cfg->desc_addr, value);
            break;
        case VIRTIO_MMIO_REG(QueueDescHigh):
            ret = virtio_mmio_write_queue_addr_high(&cfg->desc_addr, value);
            break;
        case VIRTIO_MMIO_REG(QueueDriverLow):
            ret = virtio_mmio_write_queue_addr_low(&cfg->driver_addr, value);
            break;
        case VIRTIO_MMIO_REG(QueueDriverHigh):
            ret = virtio_mmio_write_queue_addr_high(&cfg->driver_addr, value);
            break;
        case VIRTIO_MMIO_REG(QueueDeviceLow):
            ret = virtio_mmio_write_queue_addr_low(&cfg->device_addr, value);
            break;
        case VIRTIO_MMIO_REG(QueueDeviceHigh):
            ret = virtio_mmio_write_queue_addr_high(&cfg->device_addr, value);
            break;
        }
        break;
    case VIRTIO_MMIO_REG(SHMSel):
    case VIRTIO_MMIO_REG(QueueReset):
        break;
    case VIRTIO_MMIO_REG(MagicValue):
    case VIRTIO_MMIO_REG(Version):
    case VIRTIO_MMIO_REG(DeviceID):
    case VIRTIO_MMIO_REG(VendorID):
    case VIRTIO_MMIO_REG(QueueNumMax):
    case VIRTIO_MMIO_REG(InterruptStatus):
    case VIRTIO_MMIO_REG(ConfigGeneration):
    case VIRTIO_MMIO_REG(SHMLenLow):
    case VIRTIO_MMIO_REG(SHMLenHigh):
    case VIRTIO_MMIO_REG(SHMBaseLow):
    case VIRTIO_MMIO_REG(SHMBaseHigh):
        ret = -EPERM;
        break;
    default:
        ret = -EINVAL;
        break;
    }

    pthread_mutex_unlock(&common->transport_lock);
    if (ret == 0 && activation_request.pending)
        ret = virtio_mmio_complete_activation(common, &activation_request);
    if (ret == 0 && notify_after_unlock)
        ret = virtio_mmio_complete_notify(common, notify_ops, notify_opaque,
                                          notify_queue, notify_generation);
    return ret;
}
