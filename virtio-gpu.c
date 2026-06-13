#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

#include "device.h"
#include "platform.h"
#include "ram_access.h"
#include "riscv.h"
#include "riscv_private.h"
#include "utils.h"
#include "virtio-actor.h"
#include "virtio-gpu.h"
#if SEMU_HAS(VIRGL)
#include "vgpu-renderer.h"
#endif
#include "virtio-mmio.h"
#include "virtio.h"

#define VIRTIO_GPU_CMD_TRACE_ENABLED 0

#define VIRTIO_GPU_EVENT_DISPLAY (1 << 0)

/* DMT usage macro */
#define EDID_BLOCK_SIZE 128U
#define DMT_BASE_WIDTH 1024U
#define DMT_BASE_HEIGHT 768U
#define DMT_BASE_PIXEL_CLOCK_10KHZ 6500U
#define DMT_BASE_H_BLANK 320U
#define DMT_BASE_H_FRONT 24U
#define DMT_BASE_H_SYNC 136U
#define DMT_BASE_V_BLANK 38U
#define DMT_BASE_V_FRONT 3U
#define DMT_BASE_V_SYNC 6U
#define VIRTIO_GPU_BACKING_ENTRY_PAGE_SIZE 4096U
#define VIRTIO_GPU_MAX_BACKING_ENTRIES \
    (RAM_SIZE / VIRTIO_GPU_BACKING_ENTRY_PAGE_SIZE + 1U)
#define DMT_BOUND_FIELD(field, max) \
    do {                            \
        if ((field) > (max))        \
            (field) = (max);        \
    } while (0)

#define PRIV(x) ((virtio_gpu_data_t *) x->priv)

#if VIRTIO_GPU_CMD_TRACE_ENABLED
#define VIRTIO_GPU_CMD_CASE(cmd, fn)                                 \
    case VIRTIO_GPU_CMD_##cmd:                                       \
        printf("(*) semu/virtio-gpu: %s\n", "VIRTIO_GPU_CMD_" #cmd); \
        g_virtio_gpu_backend.fn(vgpu, vq_desc, plen);                \
        break;
#else
#define VIRTIO_GPU_CMD_CASE(cmd, fn)                  \
    case VIRTIO_GPU_CMD_##cmd:                        \
        g_virtio_gpu_backend.fn(vgpu, vq_desc, plen); \
        break;
#endif

extern const struct virtio_gpu_cmd_backend g_virtio_gpu_backend;
static virtio_gpu_data_t virtio_gpu_data;
static bool virtio_gpu_instance_initialized;

static bool virtio_gpu_virgl_runtime_ready(void)
{
#if SEMU_HAS(VIRGL)
    /* The build gate exists, but guest-visible VirGL/blob remains disabled
     * until the renderer backend, GL owner handoff, fences, reset, and capsets
     * are all wired in this actor/common-transport branch.
     */
    return false;
#else
    return false;
#endif
}

static uint64_t virtio_gpu_device_features(void)
{
    uint64_t features = VIRTIO_GPU_F_EDID | VIRTIO_GPU_F_VERSION_1;

    if (virtio_gpu_virgl_runtime_ready()) {
        features |= VIRTIO_GPU_F_VIRGL | VIRTIO_GPU_F_RESOURCE_BLOB |
                    VIRTIO_GPU_F_CONTEXT_INIT;
    }

    return features;
}

static struct virtio_gpu_sw_display_counters
virtio_gpu_display_counters_snapshot(
    const struct virtio_gpu_sw_display_counter_storage *counters)
{
    return (struct virtio_gpu_sw_display_counters) {
        .full_frame_bytes =
            virtio_gpu_debug_counter_load(&counters->full_frame_bytes),
        .dirty_rect_bytes =
            virtio_gpu_debug_counter_load(&counters->dirty_rect_bytes),
        .queue_backpressure =
            virtio_gpu_debug_counter_load(&counters->queue_backpressure),
        .dirty_merges = virtio_gpu_debug_counter_load(&counters->dirty_merges),
        .full_resync_escalations =
            virtio_gpu_debug_counter_load(&counters->full_resync_escalations),
    };
}

struct virtio_gpu_debug_counters virtio_gpu_debug_counters(
    virtio_gpu_state_t *vgpu)
{
    struct virtio_gpu_debug_counters counters = {0};

    if (!vgpu)
        return counters;

    counters.display = virtio_gpu_display_counters_snapshot(
        &vgpu->sw_backend.display_counters);
    return counters;
}

void *virtio_gpu_mem_guest_to_host(virtio_gpu_state_t *vgpu,
                                   uint32_t addr,
                                   uint32_t size)
{
    if (addr >= RAM_SIZE || size > RAM_SIZE - addr) {
        fprintf(stderr,
                VIRTIO_GPU_LOG_PREFIX
                "%s(): guest address 0x%x size 0x%x out of bounds\n",
                __func__, addr, size);
        return NULL;
    }
    return (void *) ((uintptr_t) vgpu->ram + addr);
}

static inline unsigned virtio_gpu_status_load(virtio_gpu_state_t *vgpu)
{
    return atomic_load_explicit(&vgpu->common.status, memory_order_acquire);
}

void virtio_gpu_set_fail(virtio_gpu_state_t *vgpu)
{
    unsigned status = virtio_gpu_status_load(vgpu);

    virtio_device_common_set_needs_reset(&vgpu->common);
    if (status & VIRTIO_STATUS__DRIVER_OK)
        virtio_irq_trigger(&vgpu->common.irq, VIRTIO_INT__CONF_CHANGE);
}

bool virtio_gpu_actor_drain_current(virtio_gpu_state_t *vgpu)
{
    return vgpu && vgpu->actor_initialized &&
           virtio_actor_generation(&vgpu->actor) ==
               vgpu->actor_drain_generation;
}

bool virtio_gpu_begin_actor_completion(virtio_gpu_state_t *vgpu)
{
    return vgpu && vgpu->actor_initialized &&
           virtio_actor_begin_completion(&vgpu->actor,
                                         vgpu->actor_drain_generation);
}

int virtio_gpu_end_actor_completion(virtio_gpu_state_t *vgpu)
{
    if (!vgpu || !vgpu->actor_initialized)
        return -EINVAL;
    return virtio_actor_end_completion(&vgpu->actor);
}

static bool virtio_gpu_begin_actor_generation_completion(
    virtio_gpu_state_t *vgpu,
    uint64_t generation)
{
    bool accepted = vgpu && vgpu->actor_initialized &&
                    virtio_actor_begin_completion(&vgpu->actor, generation);

    return accepted;
}

int virtio_gpu_complete_deferred_ctrl(
    virtio_gpu_state_t *vgpu,
    const struct virtio_gpu_deferred_ctrl_completion *completion)
{
    struct virtq *queue;
    int ret;

    if (!vgpu || !completion || !vgpu->actor_initialized)
        return -EINVAL;

    ret = pthread_mutex_lock(&vgpu->common.transport_lock);
    if (ret != 0)
        return -ret;

    if (completion->common_generation != vgpu->common.generation ||
        vgpu->common.reset_in_progress ||
        completion->queue_index >= vgpu->common.num_queues) {
        pthread_mutex_unlock(&vgpu->common.transport_lock);
        return -ECANCELED;
    }

    queue = &vgpu->common.queues[completion->queue_index];
    if (!queue->ready ||
        (virtio_gpu_status_load(vgpu) & VIRTIO_STATUS__DEVICE_NEEDS_RESET)) {
        pthread_mutex_unlock(&vgpu->common.transport_lock);
        return -ECANCELED;
    }

    if (!virtio_gpu_begin_actor_generation_completion(
            vgpu, completion->actor_generation)) {
        pthread_mutex_unlock(&vgpu->common.transport_lock);
        return -ECANCELED;
    }

    ret = virtq_add_used(vgpu->common.dma, queue, completion->desc_head,
                         completion->len);
    if (ret == 0 && completion->trigger_irq &&
        !virtq_interrupt_suppressed(vgpu->common.dma, queue))
        virtio_irq_trigger(&vgpu->common.irq, VIRTIO_INT__USED_RING);

    virtio_gpu_end_actor_completion(vgpu);
    pthread_mutex_unlock(&vgpu->common.transport_lock);

    if (ret < 0)
        virtio_gpu_set_fail(vgpu);
    return ret;
}

void *virtio_gpu_get_request(virtio_gpu_state_t *vgpu,
                             struct virtq_desc *vq_desc,
                             size_t request_size)
{
    if ((vq_desc[0].flags & VIRTIO_DESC_F_WRITE) ||
        vq_desc[0].len < request_size || request_size > UINT32_MAX)
        return NULL;

    return virtio_gpu_mem_guest_to_host(vgpu, vq_desc[0].addr,
                                        (uint32_t) request_size);
}

const struct virtq_desc *virtio_gpu_get_response_desc(
    struct virtq_desc *vq_desc,
    size_t response_size)
{
    /* The common virtq adapter stores all device-readable segments first and
     * then all writable response segments. The first writable segment is the
     * response buffer for current 2D commands; a too-small writable descriptor
     * is malformed, so do not skip past it looking for another response.
     */
    if (response_size <= UINT32_MAX) {
        for (size_t i = 1; i < VIRTIO_GPU_MAX_DESC; i++) {
            if (!(vq_desc[i].flags & VIRTIO_DESC_F_WRITE))
                continue;
            if (vq_desc[i].len < response_size)
                break;
            return &vq_desc[i];
        }
    }

    return NULL;
}

uint32_t virtio_gpu_write_ctrl_response(
    virtio_gpu_state_t *vgpu,
    const struct virtio_gpu_ctrl_hdr *request,
    const struct virtq_desc *response_desc,
    uint32_t type)
{
    if (response_desc->len < sizeof(struct virtio_gpu_ctrl_hdr))
        return 0;

    struct virtio_gpu_ctrl_hdr *response = virtio_gpu_mem_guest_to_host(
        vgpu, response_desc->addr, sizeof(struct virtio_gpu_ctrl_hdr));
    if (!response)
        return 0;

    memset(response, 0, sizeof(*response));
    response->type = type;

    if (request->flags & VIRTIO_GPU_FLAG_FENCE) {
        response->flags = VIRTIO_GPU_FLAG_FENCE;
        response->fence_id = request->fence_id;
    }

    return sizeof(*response);
}

#if SEMU_HAS(VIRGL)
struct virtio_gpu_virgl_resource_state {
    uint32_t resource_id;
    uint64_t generation;
    bool unref_pending;
    bool blob_resource;
    bool backing_attached;
    bool backing_attach_pending;
    bool backing_detach_pending;
    struct virtio_gpu_virgl_resource_state *next;
};

static pthread_mutex_t virtio_gpu_virgl_resources_lock =
    PTHREAD_MUTEX_INITIALIZER;
static struct virtio_gpu_virgl_resource_state *virtio_gpu_virgl_resources;
static uint64_t virtio_gpu_virgl_next_resource_generation;

static struct virtio_gpu_virgl_resource_state *
virtio_gpu_virgl_find_live_resource_locked(uint32_t resource_id)
{
    for (struct virtio_gpu_virgl_resource_state *res =
             virtio_gpu_virgl_resources;
         res; res = res->next) {
        if (res->resource_id == resource_id && !res->unref_pending)
            return res;
    }

    return NULL;
}

static void virtio_gpu_virgl_remove_unref_tombstones_locked(
    uint32_t resource_id)
{
    struct virtio_gpu_virgl_resource_state **cursor =
        &virtio_gpu_virgl_resources;

    while (*cursor) {
        struct virtio_gpu_virgl_resource_state *res = *cursor;

        if (res->resource_id == resource_id && res->unref_pending) {
            *cursor = res->next;
            free(res);
            continue;
        }
        cursor = &res->next;
    }
}

bool virtio_gpu_virgl_resource_id_exists(uint32_t resource_id)
{
    int ret = pthread_mutex_lock(&virtio_gpu_virgl_resources_lock);
    bool exists;

    if (ret != 0)
        return false;

    exists = virtio_gpu_virgl_find_live_resource_locked(resource_id) != NULL;
    pthread_mutex_unlock(&virtio_gpu_virgl_resources_lock);
    return exists;
}

void virtio_gpu_virgl_discard_resource_unref(uint32_t resource_id)
{
    if (pthread_mutex_lock(&virtio_gpu_virgl_resources_lock) != 0)
        return;

    virtio_gpu_virgl_remove_unref_tombstones_locked(resource_id);
    pthread_mutex_unlock(&virtio_gpu_virgl_resources_lock);
}

static int virtio_gpu_virgl_reserve_resource(uint32_t resource_id,
                                             bool blob_resource,
                                             uint64_t *generation)
{
    struct virtio_gpu_virgl_resource_state *res;
    int ret = pthread_mutex_lock(&virtio_gpu_virgl_resources_lock);

    if (ret != 0)
        return -ret;
    if (virtio_gpu_virgl_find_live_resource_locked(resource_id)) {
        pthread_mutex_unlock(&virtio_gpu_virgl_resources_lock);
        return -EEXIST;
    }
    pthread_mutex_unlock(&virtio_gpu_virgl_resources_lock);

    res = calloc(1, sizeof(*res));
    if (!res)
        return -ENOMEM;

    ret = pthread_mutex_lock(&virtio_gpu_virgl_resources_lock);
    if (ret != 0) {
        free(res);
        return -ret;
    }
    if (virtio_gpu_virgl_find_live_resource_locked(resource_id)) {
        pthread_mutex_unlock(&virtio_gpu_virgl_resources_lock);
        free(res);
        return -EEXIST;
    }
    virtio_gpu_virgl_remove_unref_tombstones_locked(resource_id);

    virtio_gpu_virgl_next_resource_generation++;
    if (!virtio_gpu_virgl_next_resource_generation)
        virtio_gpu_virgl_next_resource_generation++;

    res->resource_id = resource_id;
    res->generation = virtio_gpu_virgl_next_resource_generation;
    res->blob_resource = blob_resource;
    res->next = virtio_gpu_virgl_resources;
    virtio_gpu_virgl_resources = res;
    if (generation)
        *generation = res->generation;
    pthread_mutex_unlock(&virtio_gpu_virgl_resources_lock);
    return 0;
}

static int virtio_gpu_virgl_mark_resource_unref(uint32_t resource_id,
                                                uint64_t *generation)
{
    struct virtio_gpu_virgl_resource_state *res;
    int ret = pthread_mutex_lock(&virtio_gpu_virgl_resources_lock);

    if (ret != 0)
        return -ret;

    res = virtio_gpu_virgl_find_live_resource_locked(resource_id);
    if (!res) {
        pthread_mutex_unlock(&virtio_gpu_virgl_resources_lock);
        return -ENOENT;
    }

    res->unref_pending = true;
    if (generation)
        *generation = res->generation;
    pthread_mutex_unlock(&virtio_gpu_virgl_resources_lock);
    return 0;
}

static void virtio_gpu_virgl_remove_resource_generation(uint32_t resource_id,
                                                        uint64_t generation)
{
    struct virtio_gpu_virgl_resource_state **cursor;

    if (pthread_mutex_lock(&virtio_gpu_virgl_resources_lock) != 0)
        return;

    cursor = &virtio_gpu_virgl_resources;
    while (*cursor) {
        struct virtio_gpu_virgl_resource_state *res = *cursor;

        if (res->resource_id == resource_id && res->generation == generation) {
            *cursor = res->next;
            free(res);
            pthread_mutex_unlock(&virtio_gpu_virgl_resources_lock);
            return;
        }
        cursor = &res->next;
    }

    pthread_mutex_unlock(&virtio_gpu_virgl_resources_lock);
}

static void virtio_gpu_virgl_rollback_resource_unref(uint32_t resource_id,
                                                     uint64_t generation)
{
    if (pthread_mutex_lock(&virtio_gpu_virgl_resources_lock) != 0)
        return;

    for (struct virtio_gpu_virgl_resource_state *res =
             virtio_gpu_virgl_resources;
         res; res = res->next) {
        if (res->resource_id != resource_id)
            continue;
        if (!res->unref_pending) {
            pthread_mutex_unlock(&virtio_gpu_virgl_resources_lock);
            return;
        }
        if (res->generation == generation)
            res->unref_pending = false;
        pthread_mutex_unlock(&virtio_gpu_virgl_resources_lock);
        return;
    }

    pthread_mutex_unlock(&virtio_gpu_virgl_resources_lock);
}

static int virtio_gpu_virgl_begin_attach_backing(uint32_t resource_id,
                                                 uint64_t *generation)
{
    struct virtio_gpu_virgl_resource_state *res;
    int ret = pthread_mutex_lock(&virtio_gpu_virgl_resources_lock);

    if (ret != 0)
        return -ret;

    res = virtio_gpu_virgl_find_live_resource_locked(resource_id);
    if (!res) {
        pthread_mutex_unlock(&virtio_gpu_virgl_resources_lock);
        return -ENOENT;
    }
    if (res->backing_attached || res->backing_attach_pending ||
        res->backing_detach_pending) {
        pthread_mutex_unlock(&virtio_gpu_virgl_resources_lock);
        return -EALREADY;
    }

    res->backing_attach_pending = true;
    if (generation)
        *generation = res->generation;
    pthread_mutex_unlock(&virtio_gpu_virgl_resources_lock);
    return 0;
}

static void virtio_gpu_virgl_finish_attach_backing(uint32_t resource_id,
                                                   uint64_t generation,
                                                   bool success)
{
    if (pthread_mutex_lock(&virtio_gpu_virgl_resources_lock) != 0)
        return;

    for (struct virtio_gpu_virgl_resource_state *res =
             virtio_gpu_virgl_resources;
         res; res = res->next) {
        if (res->resource_id != resource_id || res->generation != generation)
            continue;
        if (res->backing_attach_pending) {
            res->backing_attach_pending = false;
            if (success)
                res->backing_attached = true;
        }
        pthread_mutex_unlock(&virtio_gpu_virgl_resources_lock);
        return;
    }

    pthread_mutex_unlock(&virtio_gpu_virgl_resources_lock);
}

static int virtio_gpu_virgl_begin_detach_backing(uint32_t resource_id,
                                                 uint64_t *generation)
{
    struct virtio_gpu_virgl_resource_state *res;
    int ret = pthread_mutex_lock(&virtio_gpu_virgl_resources_lock);

    if (ret != 0)
        return -ret;

    res = virtio_gpu_virgl_find_live_resource_locked(resource_id);
    if (!res) {
        pthread_mutex_unlock(&virtio_gpu_virgl_resources_lock);
        return -ENOENT;
    }
    if (!res->backing_attached || res->backing_attach_pending ||
        res->backing_detach_pending) {
        pthread_mutex_unlock(&virtio_gpu_virgl_resources_lock);
        return -EALREADY;
    }

    res->backing_detach_pending = true;
    if (generation)
        *generation = res->generation;
    pthread_mutex_unlock(&virtio_gpu_virgl_resources_lock);
    return 0;
}

static void virtio_gpu_virgl_finish_detach_backing(uint32_t resource_id,
                                                   uint64_t generation,
                                                   bool success)
{
    if (pthread_mutex_lock(&virtio_gpu_virgl_resources_lock) != 0)
        return;

    for (struct virtio_gpu_virgl_resource_state *res =
             virtio_gpu_virgl_resources;
         res; res = res->next) {
        if (res->resource_id != resource_id || res->generation != generation)
            continue;
        if (res->backing_detach_pending) {
            res->backing_detach_pending = false;
            if (success)
                res->backing_attached = false;
        }
        pthread_mutex_unlock(&virtio_gpu_virgl_resources_lock);
        return;
    }

    pthread_mutex_unlock(&virtio_gpu_virgl_resources_lock);
}

static int virtio_gpu_virgl_begin_transfer_3d(uint32_t resource_id,
                                              uint64_t *generation)
{
    struct virtio_gpu_virgl_resource_state *res;
    int ret = pthread_mutex_lock(&virtio_gpu_virgl_resources_lock);

    if (ret != 0)
        return -ret;

    res = virtio_gpu_virgl_find_live_resource_locked(resource_id);
    if (!res) {
        pthread_mutex_unlock(&virtio_gpu_virgl_resources_lock);
        return -ENOENT;
    }
    if (!res->backing_attached || res->backing_attach_pending ||
        res->backing_detach_pending) {
        pthread_mutex_unlock(&virtio_gpu_virgl_resources_lock);
        return -EALREADY;
    }

    if (generation)
        *generation = res->generation;
    pthread_mutex_unlock(&virtio_gpu_virgl_resources_lock);
    return 0;
}

static int virtio_gpu_virgl_begin_blob_command(uint32_t resource_id,
                                               uint64_t *generation)
{
    struct virtio_gpu_virgl_resource_state *res;
    int ret = pthread_mutex_lock(&virtio_gpu_virgl_resources_lock);

    if (ret != 0)
        return -ret;

    res = virtio_gpu_virgl_find_live_resource_locked(resource_id);
    if (!res) {
        pthread_mutex_unlock(&virtio_gpu_virgl_resources_lock);
        return -ENOENT;
    }
    if (!res->blob_resource) {
        pthread_mutex_unlock(&virtio_gpu_virgl_resources_lock);
        return -EINVAL;
    }
    if (res->backing_attach_pending || res->backing_detach_pending) {
        pthread_mutex_unlock(&virtio_gpu_virgl_resources_lock);
        return -EALREADY;
    }

    if (generation)
        *generation = res->generation;
    pthread_mutex_unlock(&virtio_gpu_virgl_resources_lock);
    return 0;
}
static void virtio_gpu_virgl_clear_resources(void)
{
    if (pthread_mutex_lock(&virtio_gpu_virgl_resources_lock) != 0)
        return;

    while (virtio_gpu_virgl_resources) {
        struct virtio_gpu_virgl_resource_state *res =
            virtio_gpu_virgl_resources;
        virtio_gpu_virgl_resources = res->next;
        free(res);
    }
    virtio_gpu_virgl_next_resource_generation = 0;
    pthread_mutex_unlock(&virtio_gpu_virgl_resources_lock);
}

static int virtio_gpu_submit_renderer_reset(uint64_t generation)
{
    struct vgpu_renderer_request request = {
        .type = VGPU_RENDERER_REQ_RESET,
        .token = {.generation = generation},
    };

    return vgpu_renderer_submit(&request) ? 0 : -EIO;
}

void virtio_gpu_virgl_apply_renderer_side_effect(
    virtio_gpu_state_t *vgpu UNUSED,
    const struct vgpu_renderer_completion *completion)
{
    if (!completion)
        return;

    switch (completion->virgl_resource.type) {
    case VGPU_VIRGL_RESOURCE_SIDE_EFFECT_CREATE_3D_ROLLBACK:
    case VGPU_VIRGL_RESOURCE_SIDE_EFFECT_UNREF:
        virtio_gpu_virgl_remove_resource_generation(
            completion->virgl_resource.resource_id,
            completion->virgl_resource.resource_generation);
        break;
    case VGPU_VIRGL_RESOURCE_SIDE_EFFECT_UNREF_ROLLBACK:
        virtio_gpu_virgl_rollback_resource_unref(
            completion->virgl_resource.resource_id,
            completion->virgl_resource.resource_generation);
        break;
    case VGPU_VIRGL_RESOURCE_SIDE_EFFECT_ATTACH_BACKING:
        virtio_gpu_virgl_finish_attach_backing(
            completion->virgl_resource.resource_id,
            completion->virgl_resource.resource_generation,
            completion->virgl_resource.backing_transition_success);
        break;
    case VGPU_VIRGL_RESOURCE_SIDE_EFFECT_DETACH_BACKING:
        virtio_gpu_virgl_finish_detach_backing(
            completion->virgl_resource.resource_id,
            completion->virgl_resource.resource_generation,
            completion->virgl_resource.backing_transition_success);
        break;
    case VGPU_VIRGL_RESOURCE_SIDE_EFFECT_NONE:
    default:
        break;
    }
}

static void virtio_gpu_copy_renderer_ctrl_cmd(
    struct vgpu_renderer_ctrl_payload *payload,
    uint32_t command_type,
    const void *request,
    size_t request_size)
{
    switch (command_type) {
    case VIRTIO_GPU_CMD_GET_CAPSET_INFO:
        memcpy(&payload->cmd.get_capset_info, request, request_size);
        break;
    case VIRTIO_GPU_CMD_GET_CAPSET:
        memcpy(&payload->cmd.get_capset, request, request_size);
        break;
    case VIRTIO_GPU_CMD_CTX_CREATE:
        memcpy(&payload->cmd.ctx_create, request, request_size);
        break;
    case VIRTIO_GPU_CMD_CTX_DESTROY:
        memcpy(&payload->cmd.ctx_destroy, request, request_size);
        break;
    case VIRTIO_GPU_CMD_RESOURCE_UNREF:
        memcpy(&payload->cmd.resource_unref, request, request_size);
        break;
    case VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING:
        memcpy(&payload->cmd.resource_attach_backing, request, request_size);
        break;
    case VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING:
        memcpy(&payload->cmd.resource_detach_backing, request, request_size);
        break;
    case VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB:
        memcpy(&payload->cmd.resource_create_blob, request, request_size);
        break;
    case VIRTIO_GPU_CMD_RESOURCE_MAP_BLOB:
        memcpy(&payload->cmd.resource_map_blob, request, request_size);
        break;
    case VIRTIO_GPU_CMD_RESOURCE_UNMAP_BLOB:
        memcpy(&payload->cmd.resource_unmap_blob, request, request_size);
        break;
    case VIRTIO_GPU_CMD_RESOURCE_CREATE_3D:
        memcpy(&payload->cmd.resource_create_3d, request, request_size);
        break;
    case VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D:
    case VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D:
        memcpy(&payload->cmd.transfer_3d, request, request_size);
        break;
    case VIRTIO_GPU_CMD_SUBMIT_3D:
        memcpy(&payload->cmd.submit_3d, request, request_size);
        break;
    }
}

static void virtio_gpu_release_renderer_ctrl_payload(void *payload)
{
    struct vgpu_renderer_ctrl_payload *ctrl = payload;

    if (!ctrl)
        return;
    free(ctrl->iov);
    free(ctrl->submit_data);
    free(ctrl);
}

static void virtio_gpu_submit_renderer_ctrl_with_iov(
    virtio_gpu_state_t *vgpu,
    struct virtq_desc *vq_desc,
    const struct virtio_gpu_ctrl_hdr *request,
    size_t request_size,
    size_t response_size,
    uint32_t command_type,
    uint32_t success_response_type,
    uint64_t resource_generation,
    struct iovec *iov,
    uint32_t iov_count,
    void *submit_data,
    size_t submit_data_size,
    uint32_t *plen)
{
    const struct virtq_desc *response_desc;
    struct vgpu_renderer_ctrl_payload *payload;
    struct vgpu_renderer_request renderer_request;

    if (!request) {
        free(iov);
        free(submit_data);
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    response_desc = virtio_gpu_get_response_desc(vq_desc, response_size);
    if (!response_desc) {
        free(iov);
        free(submit_data);
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    if (!vgpu->ctrl_dispatch.active) {
        free(iov);
        free(submit_data);
        *plen = virtio_gpu_write_ctrl_response(vgpu, request, response_desc,
                                               VIRTIO_GPU_RESP_ERR_UNSPEC);
        if (!*plen)
            virtio_gpu_set_fail(vgpu);
        return;
    }

    payload = calloc(1, sizeof(*payload));
    if (!payload) {
        free(iov);
        free(submit_data);
        *plen = virtio_gpu_write_ctrl_response(
            vgpu, request, response_desc, VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY);
        if (!*plen)
            virtio_gpu_set_fail(vgpu);
        return;
    }

    payload->hdr = *request;
    payload->hdr.type = command_type;
    virtio_gpu_copy_renderer_ctrl_cmd(payload, command_type, request,
                                      request_size);
    payload->iov = iov;
    payload->iov_count = iov_count;
    payload->submit_data = submit_data;
    payload->submit_data_size = submit_data_size;
    payload->resource_generation = resource_generation;
    payload->response_capacity = response_desc->len;
    payload->response_type = success_response_type;
    payload->ctrl_completion = (struct virtio_gpu_deferred_ctrl_completion) {
        .queue_index = vgpu->ctrl_dispatch.queue_index,
        .desc_head = vgpu->ctrl_dispatch.desc_head,
        .actor_generation = vgpu->ctrl_dispatch.actor_generation,
        .common_generation = vgpu->ctrl_dispatch.common_generation,
        .trigger_irq = vgpu->ctrl_dispatch.trigger_irq,
    };
    payload->response_desc = *response_desc;

    renderer_request = (struct vgpu_renderer_request) {
        .type = VGPU_RENDERER_REQ_CTRL,
        .token = {.generation = vgpu->ctrl_dispatch.common_generation},
        .command_type = command_type,
        .payload = payload,
        .payload_size = sizeof(*payload),
        .release_payload = virtio_gpu_release_renderer_ctrl_payload,
    };
    if (!vgpu_renderer_submit(&renderer_request)) {
        *plen = virtio_gpu_write_ctrl_response(
            vgpu, &payload->hdr, response_desc, VIRTIO_GPU_RESP_ERR_UNSPEC);
        virtio_gpu_release_renderer_ctrl_payload(payload);
        if (!*plen)
            virtio_gpu_set_fail(vgpu);
        return;
    }

    *plen = VIRTIO_GPU_RESPONSE_DEFERRED;
}

static void virtio_gpu_submit_renderer_ctrl(
    virtio_gpu_state_t *vgpu,
    struct virtq_desc *vq_desc,
    const struct virtio_gpu_ctrl_hdr *request,
    size_t request_size,
    size_t response_size,
    uint32_t command_type,
    uint32_t success_response_type,
    uint64_t resource_generation,
    uint32_t *plen)
{
    virtio_gpu_submit_renderer_ctrl_with_iov(
        vgpu, vq_desc, request, request_size, response_size, command_type,
        success_response_type, resource_generation, NULL, 0, NULL, 0, plen);
}

void virtio_gpu_virgl_get_capset_info_handler(virtio_gpu_state_t *vgpu,
                                              struct virtq_desc *vq_desc,
                                              uint32_t *plen)
{
    const struct virtio_gpu_get_capset_info *request = virtio_gpu_get_request(
        vgpu, vq_desc, sizeof(struct virtio_gpu_get_capset_info));
    if (!request) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    struct virtio_gpu_get_capset_info snapshot = *request;
    virtio_gpu_submit_renderer_ctrl(vgpu, vq_desc, &snapshot.hdr,
                                    sizeof(snapshot),
                                    sizeof(struct virtio_gpu_resp_capset_info),
                                    VIRTIO_GPU_CMD_GET_CAPSET_INFO,
                                    VIRTIO_GPU_RESP_OK_CAPSET_INFO, 0, plen);
}

void virtio_gpu_virgl_get_capset_handler(virtio_gpu_state_t *vgpu,
                                         struct virtq_desc *vq_desc,
                                         uint32_t *plen)
{
    const struct virtio_gpu_get_capset *request = virtio_gpu_get_request(
        vgpu, vq_desc, sizeof(struct virtio_gpu_get_capset));
    if (!request) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    struct virtio_gpu_get_capset snapshot = *request;
    virtio_gpu_submit_renderer_ctrl(
        vgpu, vq_desc, &snapshot.hdr, sizeof(snapshot),
        sizeof(struct virtio_gpu_resp_capset), VIRTIO_GPU_CMD_GET_CAPSET,
        VIRTIO_GPU_RESP_OK_CAPSET, 0, plen);
}

void virtio_gpu_virgl_ctx_create_handler(virtio_gpu_state_t *vgpu,
                                         struct virtq_desc *vq_desc,
                                         uint32_t *plen)
{
    const struct virtio_gpu_ctx_create *request = virtio_gpu_get_request(
        vgpu, vq_desc, sizeof(struct virtio_gpu_ctx_create));
    const struct virtq_desc *response_desc;

    if (!request) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    struct virtio_gpu_ctx_create snapshot = *request;
    response_desc = virtio_gpu_get_response_desc(
        vq_desc, sizeof(struct virtio_gpu_ctrl_hdr));
    if (!response_desc) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    if (snapshot.nlen > sizeof(snapshot.debug_name)) {
        *plen = virtio_gpu_write_ctrl_response(
            vgpu, &snapshot.hdr, response_desc,
            VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
        if (!*plen)
            virtio_gpu_set_fail(vgpu);
        return;
    }

    if (snapshot.context_init) {
        *plen = virtio_gpu_write_ctrl_response(
            vgpu, &snapshot.hdr, response_desc, VIRTIO_GPU_RESP_ERR_UNSPEC);
        if (!*plen)
            virtio_gpu_set_fail(vgpu);
        return;
    }

    virtio_gpu_submit_renderer_ctrl(
        vgpu, vq_desc, &snapshot.hdr, sizeof(snapshot),
        sizeof(struct virtio_gpu_ctrl_hdr), VIRTIO_GPU_CMD_CTX_CREATE,
        VIRTIO_GPU_RESP_OK_NODATA, 0, plen);
}

void virtio_gpu_virgl_ctx_destroy_handler(virtio_gpu_state_t *vgpu,
                                          struct virtq_desc *vq_desc,
                                          uint32_t *plen)
{
    const struct virtio_gpu_ctx_destroy *request = virtio_gpu_get_request(
        vgpu, vq_desc, sizeof(struct virtio_gpu_ctx_destroy));
    if (!request) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    struct virtio_gpu_ctx_destroy snapshot = *request;
    virtio_gpu_submit_renderer_ctrl(
        vgpu, vq_desc, &snapshot.hdr, sizeof(snapshot),
        sizeof(struct virtio_gpu_ctrl_hdr), VIRTIO_GPU_CMD_CTX_DESTROY,
        VIRTIO_GPU_RESP_OK_NODATA, 0, plen);
}

void virtio_gpu_virgl_resource_create_3d_handler(virtio_gpu_state_t *vgpu,
                                                 struct virtq_desc *vq_desc,
                                                 uint32_t *plen)
{
    const struct virtio_gpu_resource_create_3d *request =
        virtio_gpu_get_request(vgpu, vq_desc,
                               sizeof(struct virtio_gpu_resource_create_3d));
    const struct virtq_desc *response_desc;
    uint64_t resource_generation = 0;

    if (!request) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    struct virtio_gpu_resource_create_3d snapshot = *request;
    response_desc = virtio_gpu_get_response_desc(
        vq_desc, sizeof(struct virtio_gpu_ctrl_hdr));
    if (!response_desc) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    if (snapshot.resource_id == 0 ||
        virtio_gpu_sw_resource_2d_exists(vgpu, snapshot.resource_id)) {
        *plen = virtio_gpu_write_ctrl_response(
            vgpu, &snapshot.hdr, response_desc,
            VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID);
        if (!*plen)
            virtio_gpu_set_fail(vgpu);
        return;
    }

    int reserve_ret = virtio_gpu_virgl_reserve_resource(
        snapshot.resource_id, false, &resource_generation);
    if (reserve_ret == -EEXIST) {
        *plen = virtio_gpu_write_ctrl_response(
            vgpu, &snapshot.hdr, response_desc,
            VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID);
        if (!*plen)
            virtio_gpu_set_fail(vgpu);
        return;
    }
    if (reserve_ret != 0) {
        *plen =
            virtio_gpu_write_ctrl_response(vgpu, &snapshot.hdr, response_desc,
                                           VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY);
        if (!*plen)
            virtio_gpu_set_fail(vgpu);
        return;
    }

    virtio_gpu_submit_renderer_ctrl(
        vgpu, vq_desc, &snapshot.hdr, sizeof(snapshot),
        sizeof(struct virtio_gpu_ctrl_hdr), VIRTIO_GPU_CMD_RESOURCE_CREATE_3D,
        VIRTIO_GPU_RESP_OK_NODATA, resource_generation, plen);
    if (*plen != VIRTIO_GPU_RESPONSE_DEFERRED)
        virtio_gpu_virgl_remove_resource_generation(snapshot.resource_id,
                                                    resource_generation);
}

static size_t virtio_gpu_readable_desc_bytes(struct virtq_desc *vq_desc,
                                             size_t first_desc);
static bool virtio_gpu_copy_readable_descs(virtio_gpu_state_t *vgpu,
                                           struct virtq_desc *vq_desc,
                                           size_t first_desc,
                                           void *dst,
                                           size_t bytes);

static bool virtio_gpu_virgl_blob_create_params_valid(
    const struct virtio_gpu_resource_create_blob *request)
{
    uint32_t known_flags = VIRTIO_GPU_BLOB_FLAG_USE_MAPPABLE |
                           VIRTIO_GPU_BLOB_FLAG_USE_SHAREABLE |
                           VIRTIO_GPU_BLOB_FLAG_USE_CROSS_DEVICE;

    if (request->blob_flags & ~known_flags)
        return false;

    switch (request->blob_mem) {
    case VIRTIO_GPU_BLOB_MEM_GUEST:
    case VIRTIO_GPU_BLOB_MEM_HOST3D:
    case VIRTIO_GPU_BLOB_MEM_HOST3D_GUEST:
        return true;
    default:
        return false;
    }
}

static int virtio_gpu_virgl_build_blob_iov(
    virtio_gpu_state_t *vgpu,
    struct virtq_desc *vq_desc,
    const struct virtio_gpu_resource_create_blob *request,
    struct iovec **iov_out)
{
    struct virtio_gpu_mem_entry *entries;
    struct iovec *iov;
    size_t entries_size;

    *iov_out = NULL;
    if (request->nr_entries == 0)
        return 0;
    if (request->nr_entries > VIRTIO_GPU_MAX_BACKING_ENTRIES)
        return -EINVAL;
    entries_size = sizeof(struct virtio_gpu_mem_entry) * request->nr_entries;
    if (virtio_gpu_readable_desc_bytes(vq_desc, 1) < entries_size)
        return -EINVAL;

    entries = malloc(entries_size);
    if (!entries)
        return -ENOMEM;
    if (!virtio_gpu_copy_readable_descs(vgpu, vq_desc, 1, entries,
                                        entries_size)) {
        free(entries);
        return -EFAULT;
    }

    iov = calloc(request->nr_entries, sizeof(*iov));
    if (!iov) {
        free(entries);
        return -ENOMEM;
    }

    for (uint32_t i = 0; i < request->nr_entries; i++) {
        if (entries[i].addr > UINT32_MAX) {
            free(iov);
            free(entries);
            return -EINVAL;
        }

        iov[i].iov_base = virtio_gpu_mem_guest_to_host(
            vgpu, (uint32_t) entries[i].addr, entries[i].length);
        iov[i].iov_len = entries[i].length;
        if (!iov[i].iov_base) {
            free(iov);
            free(entries);
            return -EINVAL;
        }
    }

    free(entries);
    *iov_out = iov;
    return 0;
}

void virtio_gpu_virgl_resource_create_blob_handler(virtio_gpu_state_t *vgpu,
                                                   struct virtq_desc *vq_desc,
                                                   uint32_t *plen)
{
    const struct virtio_gpu_resource_create_blob *request =
        virtio_gpu_get_request(vgpu, vq_desc,
                               sizeof(struct virtio_gpu_resource_create_blob));
    const struct virtq_desc *response_desc;
    struct iovec *iov = NULL;
    uint64_t resource_generation = 0;
    int ret;

    if (!request) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    struct virtio_gpu_resource_create_blob snapshot = *request;
    response_desc = virtio_gpu_get_response_desc(
        vq_desc, sizeof(struct virtio_gpu_ctrl_hdr));
    if (!response_desc) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    if (snapshot.resource_id == 0 ||
        virtio_gpu_sw_resource_2d_exists(vgpu, snapshot.resource_id)) {
        *plen = virtio_gpu_write_ctrl_response(
            vgpu, &snapshot.hdr, response_desc,
            VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID);
        if (!*plen)
            virtio_gpu_set_fail(vgpu);
        return;
    }
    if (!virtio_gpu_virgl_blob_create_params_valid(&snapshot)) {
        *plen = virtio_gpu_write_ctrl_response(
            vgpu, &snapshot.hdr, response_desc,
            VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
        if (!*plen)
            virtio_gpu_set_fail(vgpu);
        return;
    }

    ret = virtio_gpu_virgl_build_blob_iov(vgpu, vq_desc, &snapshot, &iov);
    if (ret == -EINVAL) {
        *plen = virtio_gpu_write_ctrl_response(
            vgpu, &snapshot.hdr, response_desc,
            VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
        if (!*plen)
            virtio_gpu_set_fail(vgpu);
        return;
    }
    if (ret == -ENOMEM) {
        *plen =
            virtio_gpu_write_ctrl_response(vgpu, &snapshot.hdr, response_desc,
                                           VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY);
        if (!*plen)
            virtio_gpu_set_fail(vgpu);
        return;
    }
    if (ret != 0) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    int reserve_ret = virtio_gpu_virgl_reserve_resource(
        snapshot.resource_id, true, &resource_generation);
    if (reserve_ret == -EEXIST) {
        free(iov);
        *plen = virtio_gpu_write_ctrl_response(
            vgpu, &snapshot.hdr, response_desc,
            VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID);
        if (!*plen)
            virtio_gpu_set_fail(vgpu);
        return;
    }
    if (reserve_ret != 0) {
        free(iov);
        *plen =
            virtio_gpu_write_ctrl_response(vgpu, &snapshot.hdr, response_desc,
                                           VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY);
        if (!*plen)
            virtio_gpu_set_fail(vgpu);
        return;
    }

    virtio_gpu_submit_renderer_ctrl_with_iov(
        vgpu, vq_desc, &snapshot.hdr, sizeof(snapshot),
        sizeof(struct virtio_gpu_ctrl_hdr), VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB,
        VIRTIO_GPU_RESP_OK_NODATA, resource_generation, iov,
        snapshot.nr_entries, NULL, 0, plen);
    if (*plen != VIRTIO_GPU_RESPONSE_DEFERRED)
        virtio_gpu_virgl_remove_resource_generation(snapshot.resource_id,
                                                    resource_generation);
}

void virtio_gpu_virgl_resource_unref_handler(virtio_gpu_state_t *vgpu,
                                             struct virtq_desc *vq_desc,
                                             uint32_t *plen)
{
    const struct virtio_gpu_res_unref *request = virtio_gpu_get_request(
        vgpu, vq_desc, sizeof(struct virtio_gpu_res_unref));
    const struct virtq_desc *response_desc;
    uint64_t resource_generation = 0;

    if (!request) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    struct virtio_gpu_res_unref snapshot = *request;
    response_desc = virtio_gpu_get_response_desc(
        vq_desc, sizeof(struct virtio_gpu_ctrl_hdr));
    if (!response_desc) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    int unref_ret = virtio_gpu_virgl_mark_resource_unref(snapshot.resource_id,
                                                         &resource_generation);
    if (unref_ret == -ENOENT) {
        *plen = virtio_gpu_write_ctrl_response(
            vgpu, &snapshot.hdr, response_desc,
            VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID);
        if (!*plen)
            virtio_gpu_set_fail(vgpu);
        return;
    }
    if (unref_ret != 0) {
        *plen = virtio_gpu_write_ctrl_response(
            vgpu, &snapshot.hdr, response_desc, VIRTIO_GPU_RESP_ERR_UNSPEC);
        if (!*plen)
            virtio_gpu_set_fail(vgpu);
        return;
    }

    virtio_gpu_submit_renderer_ctrl(
        vgpu, vq_desc, &snapshot.hdr, sizeof(snapshot),
        sizeof(struct virtio_gpu_ctrl_hdr), VIRTIO_GPU_CMD_RESOURCE_UNREF,
        VIRTIO_GPU_RESP_OK_NODATA, resource_generation, plen);
    if (*plen != VIRTIO_GPU_RESPONSE_DEFERRED)
        virtio_gpu_virgl_rollback_resource_unref(snapshot.resource_id,
                                                 resource_generation);
}

void virtio_gpu_virgl_resource_map_blob_handler(virtio_gpu_state_t *vgpu,
                                                struct virtq_desc *vq_desc,
                                                uint32_t *plen)
{
    const struct virtio_gpu_resource_map_blob *request = virtio_gpu_get_request(
        vgpu, vq_desc, sizeof(struct virtio_gpu_resource_map_blob));
    const struct virtq_desc *response_desc;
    uint64_t resource_generation = 0;
    int ret;

    if (!request) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    struct virtio_gpu_resource_map_blob snapshot = *request;
    response_desc = virtio_gpu_get_response_desc(
        vq_desc, sizeof(struct virtio_gpu_resp_map_info));
    if (!response_desc) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    ret = virtio_gpu_virgl_begin_blob_command(snapshot.resource_id,
                                              &resource_generation);
    if (ret == -ENOENT) {
        *plen = virtio_gpu_write_ctrl_response(
            vgpu, &snapshot.hdr, response_desc,
            VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID);
        if (!*plen)
            virtio_gpu_set_fail(vgpu);
        return;
    }
    if (ret == -EINVAL) {
        *plen = virtio_gpu_write_ctrl_response(
            vgpu, &snapshot.hdr, response_desc,
            VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
        if (!*plen)
            virtio_gpu_set_fail(vgpu);
        return;
    }
    if (ret != 0) {
        *plen = virtio_gpu_write_ctrl_response(
            vgpu, &snapshot.hdr, response_desc, VIRTIO_GPU_RESP_ERR_UNSPEC);
        if (!*plen)
            virtio_gpu_set_fail(vgpu);
        return;
    }

    snapshot.hdr.type = VIRTIO_GPU_CMD_RESOURCE_MAP_BLOB;
    virtio_gpu_submit_renderer_ctrl(
        vgpu, vq_desc, &snapshot.hdr, sizeof(snapshot),
        sizeof(struct virtio_gpu_resp_map_info),
        VIRTIO_GPU_CMD_RESOURCE_MAP_BLOB, VIRTIO_GPU_RESP_OK_MAP_INFO,
        resource_generation, plen);
}

void virtio_gpu_virgl_resource_unmap_blob_handler(virtio_gpu_state_t *vgpu,
                                                  struct virtq_desc *vq_desc,
                                                  uint32_t *plen)
{
    const struct virtio_gpu_resource_unmap_blob *request =
        virtio_gpu_get_request(vgpu, vq_desc,
                               sizeof(struct virtio_gpu_resource_unmap_blob));
    const struct virtq_desc *response_desc;
    uint64_t resource_generation = 0;
    int ret;

    if (!request) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    struct virtio_gpu_resource_unmap_blob snapshot = *request;
    response_desc = virtio_gpu_get_response_desc(
        vq_desc, sizeof(struct virtio_gpu_ctrl_hdr));
    if (!response_desc) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    ret = virtio_gpu_virgl_begin_blob_command(snapshot.resource_id,
                                              &resource_generation);
    if (ret == -ENOENT) {
        *plen = virtio_gpu_write_ctrl_response(
            vgpu, &snapshot.hdr, response_desc,
            VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID);
        if (!*plen)
            virtio_gpu_set_fail(vgpu);
        return;
    }
    if (ret == -EINVAL) {
        *plen = virtio_gpu_write_ctrl_response(
            vgpu, &snapshot.hdr, response_desc,
            VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
        if (!*plen)
            virtio_gpu_set_fail(vgpu);
        return;
    }
    if (ret != 0) {
        *plen = virtio_gpu_write_ctrl_response(
            vgpu, &snapshot.hdr, response_desc, VIRTIO_GPU_RESP_ERR_UNSPEC);
        if (!*plen)
            virtio_gpu_set_fail(vgpu);
        return;
    }

    snapshot.hdr.type = VIRTIO_GPU_CMD_RESOURCE_UNMAP_BLOB;
    virtio_gpu_submit_renderer_ctrl(
        vgpu, vq_desc, &snapshot.hdr, sizeof(snapshot),
        sizeof(struct virtio_gpu_ctrl_hdr), VIRTIO_GPU_CMD_RESOURCE_UNMAP_BLOB,
        VIRTIO_GPU_RESP_OK_NODATA, resource_generation, plen);
}

static size_t virtio_gpu_readable_desc_bytes(struct virtq_desc *vq_desc,
                                             size_t first_desc)
{
    size_t total = 0;

    for (size_t i = first_desc; i < VIRTIO_GPU_MAX_DESC; i++) {
        if (vq_desc[i].flags & VIRTIO_DESC_F_WRITE)
            break;
        if (vq_desc[i].len > SIZE_MAX - total)
            return SIZE_MAX;
        total += vq_desc[i].len;
    }

    return total;
}

static bool virtio_gpu_copy_readable_descs(virtio_gpu_state_t *vgpu,
                                           struct virtq_desc *vq_desc,
                                           size_t first_desc,
                                           void *dst,
                                           size_t bytes)
{
    size_t done = 0;

    for (size_t i = first_desc; i < VIRTIO_GPU_MAX_DESC && done < bytes; i++) {
        size_t chunk;
        void *src;

        if (vq_desc[i].flags & VIRTIO_DESC_F_WRITE)
            break;
        if (vq_desc[i].addr > UINT32_MAX)
            return false;

        chunk = MIN((size_t) vq_desc[i].len, bytes - done);
        if (chunk > UINT32_MAX)
            return false;

        src = virtio_gpu_mem_guest_to_host(vgpu, (uint32_t) vq_desc[i].addr,
                                           (uint32_t) chunk);
        if (!src)
            return false;

        memcpy((uint8_t *) dst + done, src, chunk);
        done += chunk;
    }

    return done == bytes;
}

enum virtio_gpu_readable_copy_result {
    VIRTIO_GPU_READABLE_COPY_OK = 0,
    VIRTIO_GPU_READABLE_COPY_SHORT,
    VIRTIO_GPU_READABLE_COPY_FATAL,
};

static size_t virtio_gpu_readable_desc_bytes_after(struct virtq_desc *vq_desc,
                                                   size_t skip)
{
    size_t total = 0;

    for (size_t i = 0; i < VIRTIO_GPU_MAX_DESC; i++) {
        size_t desc_offset;
        size_t available;

        if (vq_desc[i].flags & VIRTIO_DESC_F_WRITE)
            break;
        if (skip >= vq_desc[i].len) {
            skip -= vq_desc[i].len;
            continue;
        }

        desc_offset = skip;
        skip = 0;
        available = (size_t) vq_desc[i].len - desc_offset;
        if (available > SIZE_MAX - total)
            return SIZE_MAX;
        total += available;
    }

    return total;
}

static enum virtio_gpu_readable_copy_result
virtio_gpu_copy_readable_descs_after(virtio_gpu_state_t *vgpu,
                                     struct virtq_desc *vq_desc,
                                     size_t skip,
                                     void *dst,
                                     size_t bytes)
{
    size_t done = 0;

    for (size_t i = 0; i < VIRTIO_GPU_MAX_DESC && done < bytes; i++) {
        size_t desc_offset = 0;
        size_t available;
        size_t chunk;
        void *src;

        if (vq_desc[i].flags & VIRTIO_DESC_F_WRITE)
            break;
        if (skip >= vq_desc[i].len) {
            skip -= vq_desc[i].len;
            continue;
        }

        desc_offset = skip;
        skip = 0;
        if (desc_offset > UINT32_MAX ||
            vq_desc[i].addr > UINT32_MAX - (uint32_t) desc_offset)
            return VIRTIO_GPU_READABLE_COPY_FATAL;

        available = (size_t) vq_desc[i].len - desc_offset;
        if (available == 0)
            continue;
        chunk = MIN(available, bytes - done);
        if (chunk > UINT32_MAX)
            return VIRTIO_GPU_READABLE_COPY_FATAL;

        src = virtio_gpu_mem_guest_to_host(
            vgpu, (uint32_t) (vq_desc[i].addr + desc_offset), (uint32_t) chunk);
        if (!src)
            return VIRTIO_GPU_READABLE_COPY_FATAL;

        memcpy((uint8_t *) dst + done, src, chunk);
        done += chunk;
    }

    return done == bytes ? VIRTIO_GPU_READABLE_COPY_OK
                         : VIRTIO_GPU_READABLE_COPY_SHORT;
}

static int virtio_gpu_virgl_build_backing_iov(
    virtio_gpu_state_t *vgpu,
    struct virtq_desc *vq_desc,
    const struct virtio_gpu_res_attach_backing *request,
    struct iovec **iov_out)
{
    struct virtio_gpu_mem_entry *entries;
    struct iovec *iov;
    size_t entries_size;

    *iov_out = NULL;
    if (request->nr_entries == 0 ||
        request->nr_entries > VIRTIO_GPU_MAX_BACKING_ENTRIES)
        return -EINVAL;
    entries_size = sizeof(struct virtio_gpu_mem_entry) * request->nr_entries;
    if (virtio_gpu_readable_desc_bytes(vq_desc, 1) < entries_size)
        return -EINVAL;

    entries = malloc(entries_size);
    if (!entries)
        return -ENOMEM;
    if (!virtio_gpu_copy_readable_descs(vgpu, vq_desc, 1, entries,
                                        entries_size)) {
        free(entries);
        return -EFAULT;
    }

    iov = calloc(request->nr_entries, sizeof(*iov));
    if (!iov) {
        free(entries);
        return -ENOMEM;
    }

    for (uint32_t i = 0; i < request->nr_entries; i++) {
        if (entries[i].addr > UINT32_MAX) {
            free(iov);
            free(entries);
            return -EINVAL;
        }

        iov[i].iov_base = virtio_gpu_mem_guest_to_host(
            vgpu, (uint32_t) entries[i].addr, entries[i].length);
        iov[i].iov_len = entries[i].length;
        if (!iov[i].iov_base) {
            free(iov);
            free(entries);
            return -EINVAL;
        }
    }

    free(entries);
    *iov_out = iov;
    return 0;
}

void virtio_gpu_virgl_resource_attach_backing_handler(
    virtio_gpu_state_t *vgpu,
    struct virtq_desc *vq_desc,
    uint32_t *plen)
{
    const struct virtio_gpu_res_attach_backing *request =
        virtio_gpu_get_request(vgpu, vq_desc,
                               sizeof(struct virtio_gpu_res_attach_backing));
    const struct virtq_desc *response_desc;
    struct iovec *iov = NULL;
    uint64_t resource_generation = 0;
    int ret;

    if (!request) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    struct virtio_gpu_res_attach_backing snapshot = *request;
    response_desc = virtio_gpu_get_response_desc(
        vq_desc, sizeof(struct virtio_gpu_ctrl_hdr));
    if (!response_desc) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    if (vq_desc[1].flags & VIRTIO_DESC_F_WRITE) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    ret = virtio_gpu_virgl_build_backing_iov(vgpu, vq_desc, &snapshot, &iov);
    if (ret == -EINVAL) {
        *plen = virtio_gpu_write_ctrl_response(
            vgpu, &snapshot.hdr, response_desc,
            VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
        if (!*plen)
            virtio_gpu_set_fail(vgpu);
        return;
    }
    if (ret == -ENOMEM) {
        *plen =
            virtio_gpu_write_ctrl_response(vgpu, &snapshot.hdr, response_desc,
                                           VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY);
        if (!*plen)
            virtio_gpu_set_fail(vgpu);
        return;
    }
    if (ret != 0) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    ret = virtio_gpu_virgl_begin_attach_backing(snapshot.resource_id,
                                                &resource_generation);
    if (ret == -ENOENT) {
        free(iov);
        *plen = virtio_gpu_write_ctrl_response(
            vgpu, &snapshot.hdr, response_desc,
            VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID);
        if (!*plen)
            virtio_gpu_set_fail(vgpu);
        return;
    }
    if (ret != 0) {
        free(iov);
        *plen = virtio_gpu_write_ctrl_response(
            vgpu, &snapshot.hdr, response_desc, VIRTIO_GPU_RESP_ERR_UNSPEC);
        if (!*plen)
            virtio_gpu_set_fail(vgpu);
        return;
    }

    virtio_gpu_submit_renderer_ctrl_with_iov(
        vgpu, vq_desc, &snapshot.hdr, sizeof(snapshot),
        sizeof(struct virtio_gpu_ctrl_hdr),
        VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING, VIRTIO_GPU_RESP_OK_NODATA,
        resource_generation, iov, snapshot.nr_entries, NULL, 0, plen);
    if (*plen != VIRTIO_GPU_RESPONSE_DEFERRED)
        virtio_gpu_virgl_finish_attach_backing(snapshot.resource_id,
                                               resource_generation, false);
}

void virtio_gpu_virgl_resource_detach_backing_handler(
    virtio_gpu_state_t *vgpu,
    struct virtq_desc *vq_desc,
    uint32_t *plen)
{
    const struct virtio_gpu_res_detach_backing *request =
        virtio_gpu_get_request(vgpu, vq_desc,
                               sizeof(struct virtio_gpu_res_detach_backing));
    const struct virtq_desc *response_desc;
    uint64_t resource_generation = 0;
    int ret;

    if (!request) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    struct virtio_gpu_res_detach_backing snapshot = *request;
    response_desc = virtio_gpu_get_response_desc(
        vq_desc, sizeof(struct virtio_gpu_ctrl_hdr));
    if (!response_desc) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    ret = virtio_gpu_virgl_begin_detach_backing(snapshot.resource_id,
                                                &resource_generation);
    if (ret == -ENOENT) {
        *plen = virtio_gpu_write_ctrl_response(
            vgpu, &snapshot.hdr, response_desc,
            VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID);
        if (!*plen)
            virtio_gpu_set_fail(vgpu);
        return;
    }
    if (ret != 0) {
        *plen = virtio_gpu_write_ctrl_response(
            vgpu, &snapshot.hdr, response_desc, VIRTIO_GPU_RESP_ERR_UNSPEC);
        if (!*plen)
            virtio_gpu_set_fail(vgpu);
        return;
    }

    virtio_gpu_submit_renderer_ctrl(
        vgpu, vq_desc, &snapshot.hdr, sizeof(snapshot),
        sizeof(struct virtio_gpu_ctrl_hdr),
        VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING, VIRTIO_GPU_RESP_OK_NODATA,
        resource_generation, plen);
    if (*plen != VIRTIO_GPU_RESPONSE_DEFERRED)
        virtio_gpu_virgl_finish_detach_backing(snapshot.resource_id,
                                               resource_generation, false);
}

static void virtio_gpu_virgl_transfer_host_3d_handler(
    virtio_gpu_state_t *vgpu,
    struct virtq_desc *vq_desc,
    uint32_t *plen,
    uint32_t command_type)
{
    const struct virtio_gpu_transfer_host_3d *request = virtio_gpu_get_request(
        vgpu, vq_desc, sizeof(struct virtio_gpu_transfer_host_3d));
    const struct virtq_desc *response_desc;
    uint64_t resource_generation = 0;
    int ret;

    if (!request) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    struct virtio_gpu_transfer_host_3d snapshot = *request;
    response_desc = virtio_gpu_get_response_desc(
        vq_desc, sizeof(struct virtio_gpu_ctrl_hdr));
    if (!response_desc) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    if (snapshot.level > (uint32_t) INT_MAX) {
        *plen = virtio_gpu_write_ctrl_response(
            vgpu, &snapshot.hdr, response_desc,
            VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
        if (!*plen)
            virtio_gpu_set_fail(vgpu);
        return;
    }

    ret = virtio_gpu_virgl_begin_transfer_3d(snapshot.resource_id,
                                             &resource_generation);
    if (ret == -ENOENT) {
        *plen = virtio_gpu_write_ctrl_response(
            vgpu, &snapshot.hdr, response_desc,
            VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID);
        if (!*plen)
            virtio_gpu_set_fail(vgpu);
        return;
    }
    if (ret != 0) {
        *plen = virtio_gpu_write_ctrl_response(
            vgpu, &snapshot.hdr, response_desc, VIRTIO_GPU_RESP_ERR_UNSPEC);
        if (!*plen)
            virtio_gpu_set_fail(vgpu);
        return;
    }

    snapshot.hdr.type = command_type;
    virtio_gpu_submit_renderer_ctrl(
        vgpu, vq_desc, &snapshot.hdr, sizeof(snapshot),
        sizeof(struct virtio_gpu_ctrl_hdr), command_type,
        VIRTIO_GPU_RESP_OK_NODATA, resource_generation, plen);
}

void virtio_gpu_virgl_transfer_to_host_3d_handler(virtio_gpu_state_t *vgpu,
                                                  struct virtq_desc *vq_desc,
                                                  uint32_t *plen)
{
    virtio_gpu_virgl_transfer_host_3d_handler(
        vgpu, vq_desc, plen, VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D);
}

void virtio_gpu_virgl_transfer_from_host_3d_handler(virtio_gpu_state_t *vgpu,
                                                    struct virtq_desc *vq_desc,
                                                    uint32_t *plen)
{
    virtio_gpu_virgl_transfer_host_3d_handler(
        vgpu, vq_desc, plen, VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D);
}

void virtio_gpu_virgl_submit_3d_handler(virtio_gpu_state_t *vgpu,
                                        struct virtq_desc *vq_desc,
                                        uint32_t *plen)
{
    const struct virtio_gpu_cmd_submit *request = virtio_gpu_get_request(
        vgpu, vq_desc, sizeof(struct virtio_gpu_cmd_submit));
    const struct virtq_desc *response_desc;
    void *submit_data;
    enum virtio_gpu_readable_copy_result copy_result;

    if (!request) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    struct virtio_gpu_cmd_submit snapshot = *request;
    response_desc = virtio_gpu_get_response_desc(
        vq_desc, sizeof(struct virtio_gpu_ctrl_hdr));
    if (!response_desc) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    if (snapshot.size == 0 || snapshot.size % sizeof(uint32_t) != 0 ||
        snapshot.size / sizeof(uint32_t) > (uint32_t) INT_MAX ||
        snapshot.num_in_fences != 0) {
        *plen = virtio_gpu_write_ctrl_response(
            vgpu, &snapshot.hdr, response_desc,
            VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
        if (!*plen)
            virtio_gpu_set_fail(vgpu);
        return;
    }

    if (snapshot.hdr.flags & VIRTIO_GPU_FLAG_FENCE) {
        *plen = virtio_gpu_write_ctrl_response(
            vgpu, &snapshot.hdr, response_desc, VIRTIO_GPU_RESP_ERR_UNSPEC);
        if (!*plen)
            virtio_gpu_set_fail(vgpu);
        return;
    }

    if (virtio_gpu_readable_desc_bytes_after(
            vq_desc, sizeof(struct virtio_gpu_cmd_submit)) < snapshot.size) {
        *plen = virtio_gpu_write_ctrl_response(
            vgpu, &snapshot.hdr, response_desc,
            VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
        if (!*plen)
            virtio_gpu_set_fail(vgpu);
        return;
    }

    submit_data = malloc(snapshot.size);
    if (!submit_data) {
        *plen =
            virtio_gpu_write_ctrl_response(vgpu, &snapshot.hdr, response_desc,
                                           VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY);
        if (!*plen)
            virtio_gpu_set_fail(vgpu);
        return;
    }

    copy_result = virtio_gpu_copy_readable_descs_after(
        vgpu, vq_desc, sizeof(struct virtio_gpu_cmd_submit), submit_data,
        snapshot.size);
    if (copy_result != VIRTIO_GPU_READABLE_COPY_OK) {
        free(submit_data);
        if (copy_result == VIRTIO_GPU_READABLE_COPY_SHORT) {
            *plen = virtio_gpu_write_ctrl_response(
                vgpu, &snapshot.hdr, response_desc,
                VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
            if (!*plen)
                virtio_gpu_set_fail(vgpu);
            return;
        }

        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    virtio_gpu_submit_renderer_ctrl_with_iov(
        vgpu, vq_desc, &snapshot.hdr, sizeof(snapshot),
        sizeof(struct virtio_gpu_ctrl_hdr), VIRTIO_GPU_CMD_SUBMIT_3D,
        VIRTIO_GPU_RESP_OK_NODATA, 0, NULL, 0, submit_data, snapshot.size,
        plen);
}
#endif

/* 'virtio_gpu' protocol handlers */
void virtio_gpu_get_display_info_handler(virtio_gpu_state_t *vgpu,
                                         struct virtq_desc *vq_desc,
                                         uint32_t *plen)
{
    struct virtio_gpu_ctrl_hdr *request = virtio_gpu_get_request(
        vgpu, vq_desc, sizeof(struct virtio_gpu_ctrl_hdr));
    if (!request) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    const struct virtq_desc *response_desc = virtio_gpu_get_response_desc(
        vq_desc, sizeof(struct virtio_gpu_resp_disp_info));
    if (!response_desc) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    struct virtio_gpu_resp_disp_info *response = virtio_gpu_mem_guest_to_host(
        vgpu, response_desc->addr, sizeof(struct virtio_gpu_resp_disp_info));
    if (!response) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    memset(response, 0, sizeof(*response));
    response->hdr.type = VIRTIO_GPU_RESP_OK_DISPLAY_INFO;

    /* 'GET_DISPLAY_INFO' exposes scanouts as the 'pmodes[]' array, so the array
     * index is the guest-visible 'scanout_id' used by later requests such as
     * 'SET_SCANOUT' and 'GET_EDID'.
     *
     * The spec describes 'pmodes[]' as per-scanout information but does not
     * spell out this mapping as a separate rule. semu follows the implicit
     * model where 'pmodes[i]' describes scanout ID 'i' because later requests
     * only carry a 'scanout_id', and Linux does the same when it copies
     * 'resp->pmodes[i]' into 'outputs[i]' and later sends 'output->index' in
     * 'SET_SCANOUT'. See 'virtgpu_vq.c' and 'virtgpu_display.c' for more
     * details.
     */
    int scanout_num = PRIV(vgpu)->num_scanouts;
    for (int i = 0; i < scanout_num; i++) {
        response->pmodes[i].r.width = PRIV(vgpu)->scanouts[i].width;
        response->pmodes[i].r.height = PRIV(vgpu)->scanouts[i].height;
        response->pmodes[i].enabled = PRIV(vgpu)->scanouts[i].enabled;
    }

    if (request->flags & VIRTIO_GPU_FLAG_FENCE) {
        response->hdr.flags = VIRTIO_GPU_FLAG_FENCE;
        response->hdr.fence_id = request->fence_id;
    }
    *plen = sizeof(*response);
}

static uint8_t virtio_gpu_generate_edid_checksum(uint8_t *edid, size_t size)
{
    /* Check EDID 1.4 Section 3.11, Table 3.40 notes 2 and 3: byte 7Fh must
     * make the modulo-256 sum of all 128 base EDID bytes equal 00h.
     */
    uint8_t sum = 0;

    for (size_t i = 0; i < size; i++)
        sum += edid[i];

    return 0x100 - sum;
}

static uint16_t virtio_gpu_edid_pixels_to_mm(uint32_t pixels)
{
    /* Check EDID 1.4 Sections 3.6.2 and 3.10.2: base screen size is stored in
     * centimeters, while detailed timing image size is stored in millimeters.
     * Estimate virtual display size at 100 DPI.
     */
    uint32_t mm = ((uint64_t) pixels * 254U + 500U) / 1000U;

    if (mm == 0)
        mm = 1;
    if (mm > 4095)
        mm = 4095;

    return mm;
}

static uint8_t virtio_gpu_edid_mm_to_cm(uint16_t mm)
{
    /* Check EDID 1.4 Section 3.6.2: base screen size fields are centimeters. */
    uint32_t cm = (mm + 5U) / 10U;

    if (cm == 0)
        cm = 1;
    if (cm > 255)
        cm = 255;

    return cm;
}

static void virtio_gpu_edid_set_srgb_chromaticity(uint8_t *edid)
{
    /* Check EDID 1.4 Section 3.7: sRGB chromaticity coordinates in EDID
     * 10-bit fixed-point form, value = round(coordinate * 1024). The white
     * point is D65.
     */
    const uint16_t red_x = 655;   /* round(0.640 * 1024) */
    const uint16_t red_y = 338;   /* round(0.330 * 1024) */
    const uint16_t green_x = 307; /* round(0.300 * 1024) */
    const uint16_t green_y = 614; /* round(0.600 * 1024) */
    const uint16_t blue_x = 154;  /* round(0.150 * 1024) */
    const uint16_t blue_y = 61;   /* round(0.060 * 1024) */
    const uint16_t white_x = 320; /* round(0.313 * 1024) */
    const uint16_t white_y = 337; /* round(0.329 * 1024) */

    edid[25] = ((red_x & 0x3) << 6) | ((red_y & 0x3) << 4) |
               ((green_x & 0x3) << 2) | (green_y & 0x3);
    edid[26] = ((blue_x & 0x3) << 6) | ((blue_y & 0x3) << 4) |
               ((white_x & 0x3) << 2) | (white_y & 0x3);
    edid[27] = red_x >> 2;
    edid[28] = red_y >> 2;
    edid[29] = green_x >> 2;
    edid[30] = green_y >> 2;
    edid[31] = blue_x >> 2;
    edid[32] = blue_y >> 2;
    edid[33] = white_x >> 2;
    edid[34] = white_y >> 2;
}

static void virtio_gpu_edid_set_detailed_timing(uint8_t *desc,
                                                uint32_t width,
                                                uint32_t height,
                                                uint16_t width_mm,
                                                uint16_t height_mm)
{
    /* Check EDID 1.4 Section 3.10.2: detailed timing descriptor layout. */
    uint32_t h_blank;           /* Horizontal blanking pixels. */
    uint32_t h_front;           /* Horizontal front porch pixels. */
    uint32_t h_sync;            /* Horizontal sync pulse width. */
    uint32_t v_blank;           /* Vertical blanking lines. */
    uint32_t v_front;           /* Vertical front porch lines. */
    uint32_t v_sync;            /* Vertical sync pulse width. */
    uint32_t pixel_clock_10khz; /* Pixel clock in 10 kHz units. */

    if (width == DMT_BASE_WIDTH && height == DMT_BASE_HEIGHT) {
        /* VESA DMT 1024x768@60Hz, also advertised in the base EDID established
         * timings field. EDID stores pixel clock in 10 kHz units, so 6500
         * means 65.00 MHz.
         */
        pixel_clock_10khz = DMT_BASE_PIXEL_CLOCK_10KHZ;
        h_blank = DMT_BASE_H_BLANK;
        h_front = DMT_BASE_H_FRONT;
        h_sync = DMT_BASE_H_SYNC;
        v_blank = DMT_BASE_V_BLANK;
        v_front = DMT_BASE_V_FRONT;
        v_sync = DMT_BASE_V_SYNC;
    } else {
        /* Fallback only for future multi-mode or non-default scanouts. The
         * current machine registers one 1024x768 scanout, so this path is not
         * reachable in the default build. Scale porch/sync proportions from
         * the VESA DMT 1024x768@60Hz timing instead of inventing ad hoc
         * ratios.
         */
        h_blank = ((uint64_t) width * DMT_BASE_H_BLANK + DMT_BASE_WIDTH / 2U) /
                  DMT_BASE_WIDTH;
        h_front = ((uint64_t) width * DMT_BASE_H_FRONT + DMT_BASE_WIDTH / 2U) /
                  DMT_BASE_WIDTH;
        h_sync = ((uint64_t) width * DMT_BASE_H_SYNC + DMT_BASE_WIDTH / 2U) /
                 DMT_BASE_WIDTH;
        if (h_front == 0)
            h_front = 1;
        if (h_sync == 0)
            h_sync = 1;
        if (h_blank <= h_front + h_sync) {
            /* Keep front porch and sync pulse inside the blanking interval so
             * the remaining pixels form the back porch.
             */
            h_blank = h_front + h_sync + 1U;
        }

        v_blank =
            ((uint64_t) height * DMT_BASE_V_BLANK + DMT_BASE_HEIGHT / 2U) /
            DMT_BASE_HEIGHT;
        v_front =
            ((uint64_t) height * DMT_BASE_V_FRONT + DMT_BASE_HEIGHT / 2U) /
            DMT_BASE_HEIGHT;
        v_sync = ((uint64_t) height * DMT_BASE_V_SYNC + DMT_BASE_HEIGHT / 2U) /
                 DMT_BASE_HEIGHT;
        if (v_front == 0)
            v_front = 1;
        if (v_sync == 0)
            v_sync = 1;
        if (v_blank <= v_front + v_sync)
            v_blank = v_front + v_sync + 1U;

        /* Pixel clock = refresh rate * horizontal total * vertical total.
         * Divide by 10000 because the descriptor stores the clock in 10 kHz
         * units. The +5000 rounds to the nearest 10 kHz.
         */
        pixel_clock_10khz = (60U * ((uint64_t) width + h_blank) *
                                 ((uint64_t) height + v_blank) +
                             5000U) /
                            10000U;
        if (pixel_clock_10khz > 0xffffU)
            pixel_clock_10khz = 0xffffU;
    }

    /* Clamp fields to the bit widths defined by Table 3.21:
     * active/blanking/image-size fields are 12-bit, horizontal sync fields are
     * 10-bit, and vertical sync fields are 6-bit.
     */
    DMT_BOUND_FIELD(width, 4095U);
    DMT_BOUND_FIELD(height, 4095U);
    DMT_BOUND_FIELD(h_blank, 4095U);
    DMT_BOUND_FIELD(h_front, 1023U);
    DMT_BOUND_FIELD(h_sync, 1023U);
    DMT_BOUND_FIELD(v_blank, 4095U);
    DMT_BOUND_FIELD(v_front, 63U);
    DMT_BOUND_FIELD(v_sync, 63U);

    /* Bytes 0-1: pixel clock, little-endian, in 10 kHz units. */
    desc[0] = pixel_clock_10khz & 0xff;
    desc[1] = (pixel_clock_10khz >> 8) & 0xff;

    /* Bytes 2-4: horizontal active and blanking, each split as low 8 bits plus
     * high 4 bits packed into byte 4.
     */
    desc[2] = width & 0xff;
    desc[3] = h_blank & 0xff;
    desc[4] = ((width >> 8) << 4) | (h_blank >> 8);

    /* Bytes 5-7: vertical active and blanking, using the same 12-bit packing
     * pattern as the horizontal fields.
     */
    desc[5] = height & 0xff;
    desc[6] = v_blank & 0xff;
    desc[7] = ((height >> 8) << 4) | (v_blank >> 8);

    /* Bytes 8-11: sync offsets and pulse widths. Horizontal fields are 10-bit;
     * vertical fields are 6-bit and share byte 10 for their low nibbles.
     */
    desc[8] = h_front & 0xff;
    desc[9] = h_sync & 0xff;
    desc[10] = ((v_front & 0xf) << 4) | (v_sync & 0xf);
    desc[11] = ((h_front >> 8) << 6) | ((h_sync >> 8) << 4) |
               ((v_front >> 4) << 2) | (v_sync >> 4);

    /* Bytes 12-14: displayed image size in millimeters, again as two 12-bit
     * fields packed as low 8 bits plus high 4 bits.
     */
    desc[12] = width_mm & 0xff;
    desc[13] = height_mm & 0xff;
    desc[14] = ((width_mm >> 8) << 4) | (height_mm >> 8);

    /* Bytes 15-16: horizontal and vertical border, unused for this display. */
    desc[15] = 0;
    desc[16] = 0;

    /* Byte 17: non-interlaced, no stereo, digital separate sync, negative H/V
     * sync polarity.
     */
    desc[17] = 0x18;
}

/* EDID data follows "VESA ENHANCED EXTENDED DISPLAY IDENTIFICATION DATA
 * STANDARD" (defines EDID Structure Version 1, Revision 4).
 */
static void virtio_gpu_generate_edid(uint8_t *edid,
                                     uint32_t width,
                                     uint32_t height)
{
    /* Check EDID 1.4 Section 3.1: base EDID block layout. */
    if (width == 0)
        width = SCREEN_WIDTH;
    if (height == 0)
        height = SCREEN_HEIGHT;

    uint16_t width_mm = virtio_gpu_edid_pixels_to_mm(width);
    uint16_t height_mm = virtio_gpu_edid_pixels_to_mm(height);

    memset(edid, 0, EDID_BLOCK_SIZE);

    /* Check EDID 1.4 Section 3.3: EDID header. */
    edid[0] = 0x00;
    edid[1] = 0xff;
    edid[2] = 0xff;
    edid[3] = 0xff;
    edid[4] = 0xff;
    edid[5] = 0xff;
    edid[6] = 0xff;
    edid[7] = 0x00;

    /* Check EDID 1.4 Section 3.4.1: ID Manufacturer Name, stored as a
     * 3-character PNPID in 5-bit compressed ASCII.
     */
    char manufacture[3] = {'T', 'W', 'N'};

    /* Vendor ID uses 2 bytes to store 3 characters, where 'A' starts as 1 */
    uint16_t vendor_id = ((((manufacture[0] - '@') & 0b11111) << 10) |
                          (((manufacture[1] - '@') & 0b11111) << 5) |
                          (((manufacture[2] - '@') & 0b11111) << 0));
    /* Convert vendor ID to big-endian order */
    edid[8] = vendor_id >> 8;
    edid[9] = vendor_id & 0xff;

    /* Check EDID 1.4 Sections 3.4.2 and 3.4.3: product code and serial
     * number, all zeros if unused.
     */
    memset(&edid[10], 0, sizeof(uint16_t) + sizeof(uint32_t));

    /* Check EDID 1.4 Section 3.4.4: week of manufacture, 0 if unused. */
    edid[16] = 0;
    /* Check EDID 1.4 Section 3.4.4: year of manufacture starts from 1990. */
    edid[17] = 2023 - 1990;

    /* Check EDID 1.4 Section 3.5: version 1, revision 4. */
    edid[18] = 1; /* Version number */
    edid[19] = 4; /* Revision number */

    /* Check EDID 1.4 Section 3.6.1: video input definition. */
    uint8_t signal_interface = 0b1 << 7;  /* digital */
    uint8_t color_bit_depth = 0b010 << 4; /* 8 bits per primary color */
    uint8_t interface_type = 0b101;       /* DisplayPort is supported */
    edid[20] = signal_interface | color_bit_depth | interface_type;

    /* Check EDID 1.4 Section 3.6.2: screen size or aspect ratio. */
    edid[21] = virtio_gpu_edid_mm_to_cm(width_mm);
    edid[22] = virtio_gpu_edid_mm_to_cm(height_mm);

    /* Check EDID 1.4 Section 3.6.3: gamma value. */
    edid[23] = 120; /* 2.20 */

    /* Check EDID 1.4 Section 3.6.4: feature support. */
    uint8_t power_management = 0 << 4; /* standby, suspend and active-off
                                        * modes are not supported
                                        */
    uint8_t color_type = 0 << 3;       /* RGB 4:4:4 */
    uint8_t other_flags = 0b110;       /* [2]: sRGB as default color space
                                        * [1]: Preferred timing mode with native
                                        * format       [0]: Non-continuous frequency
                                        */
    edid[24] = power_management | color_type | other_flags;

    virtio_gpu_edid_set_srgb_chromaticity(edid);

    /* Check EDID 1.4 Section 3.8: established timings. These are the default
     * timings defined by the VESA. Each bit represents 1 configuration. For
     * now, we enable the timing configurations of 1024x768@60Hz only.
     */
    edid[35] = 0b00000000;
    edid[36] = (width == DMT_BASE_WIDTH && height == DMT_BASE_HEIGHT)
                   ? 0b00001000
                   : 0b00000000;
    edid[37] = 0b00000000;

    /* Check EDID 1.4 Section 3.9: standard timings. The 16 bytes from
     * edid[38] to edid[53] hold eight 2-byte timing identifiers. Mark every
     * standard timing slot unused.
     */
    memset(&edid[38], 0x01, 16);

    /* Check EDID 1.4 Sections 3.10.1 and 3.10.2: first detailed timing
     * descriptor is the preferred timing mode, here the native scanout mode at
     * 60Hz.
     */
    virtio_gpu_edid_set_detailed_timing(&edid[54], width, height, width_mm,
                                        height_mm);

    /* Check EDID 1.4 Sections 3.10 and 3.10.3.11: mark remaining 18-byte
     * descriptor slots unused with Dummy Descriptor tag 10h.
     */
    for (size_t desc = 72; desc < 126; desc += 18)
        edid[desc + 3] = 0x10;

    /* Check EDID 1.4 Section 3.11: extension block count. */
    edid[126] = 0; /* No other extension blocks are defined */

    /* Check EDID 1.4 Section 3.11: checksum of the base EDID block. */
    edid[EDID_BLOCK_SIZE - 1U] =
        virtio_gpu_generate_edid_checksum(edid, EDID_BLOCK_SIZE - 1U);
}

void virtio_gpu_get_edid_handler(virtio_gpu_state_t *vgpu,
                                 struct virtq_desc *vq_desc,
                                 uint32_t *plen)
{
    struct virtio_gpu_cmd_get_edid *request = virtio_gpu_get_request(
        vgpu, vq_desc, sizeof(struct virtio_gpu_cmd_get_edid));
    if (!request) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    const struct virtq_desc *response_desc = virtio_gpu_get_response_desc(
        vq_desc, sizeof(struct virtio_gpu_resp_edid));
    if (!response_desc) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    if (request->scanout >= PRIV(vgpu)->num_scanouts ||
        !PRIV(vgpu)->scanouts[request->scanout].enabled) {
        fprintf(stderr, VIRTIO_GPU_LOG_PREFIX "%s(): invalid scanout id %u\n",
                __func__, request->scanout);
        *plen = virtio_gpu_write_ctrl_response(
            vgpu, &request->hdr, response_desc,
            VIRTIO_GPU_RESP_ERR_INVALID_SCANOUT_ID);
        if (!*plen)
            virtio_gpu_set_fail(vgpu);
        return;
    }

    const struct virtio_gpu_scanout_info *scanout =
        &PRIV(vgpu)->scanouts[request->scanout];

    struct virtio_gpu_resp_edid *response = virtio_gpu_mem_guest_to_host(
        vgpu, response_desc->addr, sizeof(struct virtio_gpu_resp_edid));
    if (!response) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    memset(response, 0, sizeof(*response));
    response->hdr.type = VIRTIO_GPU_RESP_OK_EDID;
    response->size = EDID_BLOCK_SIZE; /* One base EDID block. */
    virtio_gpu_generate_edid((uint8_t *) response->edid, scanout->width,
                             scanout->height);

    if (request->hdr.flags & VIRTIO_GPU_FLAG_FENCE) {
        response->hdr.flags = VIRTIO_GPU_FLAG_FENCE;
        response->hdr.fence_id = request->hdr.fence_id;
    }
    *plen = sizeof(*response);
}

#if SEMU_HAS(VIRGL)
static void virtio_gpu_release_renderer_completion(
    struct vgpu_renderer_completion *completion)
{
    if (!completion || !completion->response)
        return;
    if (completion->release_response)
        completion->release_response(completion->response);
    else
        free(completion->response);
    completion->response = NULL;
}

static void virtio_gpu_apply_renderer_side_effect(
    virtio_gpu_state_t *vgpu,
    const struct vgpu_renderer_completion *completion)
{
    if (g_virtio_gpu_backend.apply_renderer_side_effect)
        g_virtio_gpu_backend.apply_renderer_side_effect(vgpu, completion);
}

static bool virtio_gpu_write_renderer_completion_response(
    virtio_gpu_state_t *vgpu,
    const struct vgpu_renderer_completion *completion,
    uint32_t *len)
{
    if (!completion->has_response_desc)
        return true;

    if (completion->response) {
        if (completion->response_size > completion->response_desc.len ||
            completion->response_size > UINT32_MAX)
            return false;

        void *dst =
            virtio_gpu_mem_guest_to_host(vgpu, completion->response_desc.addr,
                                         (uint32_t) completion->response_size);
        if (!dst)
            return false;

        memcpy(dst, completion->response, completion->response_size);
        *len = (uint32_t) completion->response_size;
        return true;
    }

    if (completion->response_type == 0)
        return false;

    *len = virtio_gpu_write_ctrl_response(vgpu, &completion->request_hdr,
                                          &completion->response_desc,
                                          completion->response_type);
    return *len != 0;
}

static int virtio_gpu_complete_renderer_ctrl(
    virtio_gpu_state_t *vgpu,
    const struct vgpu_renderer_completion *completion)
{
    struct virtio_gpu_deferred_ctrl_completion ctrl;
    struct virtq *queue;
    uint32_t len;
    int ret;

    if (!vgpu || !completion || !completion->has_ctrl_completion)
        return 0;

    ctrl = completion->ctrl_completion;
    len = ctrl.len;

    ret = pthread_mutex_lock(&vgpu->common.transport_lock);
    if (ret != 0)
        return -ret;

    if (ctrl.common_generation != vgpu->common.generation ||
        vgpu->common.reset_in_progress ||
        ctrl.queue_index >= vgpu->common.num_queues) {
        pthread_mutex_unlock(&vgpu->common.transport_lock);
        return -ECANCELED;
    }

    queue = &vgpu->common.queues[ctrl.queue_index];
    if (!queue->ready ||
        (virtio_gpu_status_load(vgpu) & VIRTIO_STATUS__DEVICE_NEEDS_RESET)) {
        pthread_mutex_unlock(&vgpu->common.transport_lock);
        return -ECANCELED;
    }

    if (!virtio_gpu_begin_actor_generation_completion(vgpu,
                                                      ctrl.actor_generation)) {
        pthread_mutex_unlock(&vgpu->common.transport_lock);
        return -ECANCELED;
    }

    if (!virtio_gpu_write_renderer_completion_response(vgpu, completion,
                                                       &len)) {
        virtio_gpu_end_actor_completion(vgpu);
        pthread_mutex_unlock(&vgpu->common.transport_lock);
        virtio_gpu_set_fail(vgpu);
        return -EFAULT;
    }

    virtio_gpu_apply_renderer_side_effect(vgpu, completion);

    ctrl.len = len;
    ret = virtq_add_used(vgpu->common.dma, queue, ctrl.desc_head, ctrl.len);
    if (ret == 0 && ctrl.trigger_irq &&
        !virtq_interrupt_suppressed(vgpu->common.dma, queue))
        virtio_irq_trigger(&vgpu->common.irq, VIRTIO_INT__USED_RING);

    virtio_gpu_end_actor_completion(vgpu);
    pthread_mutex_unlock(&vgpu->common.transport_lock);

    if (ret < 0)
        virtio_gpu_set_fail(vgpu);
    return ret;
}
#endif

void virtio_gpu_drain_renderer_completions(virtio_gpu_state_t *vgpu)
{
#if SEMU_HAS(VIRGL)
    struct vgpu_renderer_completion completion;

    if (!vgpu)
        return;

    while (vgpu_renderer_pop_completion(&completion)) {
        switch (completion.type) {
        case VGPU_RENDERER_DONE_CTRL:
            if (!completion.has_ctrl_completion) {
                virtio_gpu_set_fail(vgpu);
                break;
            }
            (void) virtio_gpu_complete_renderer_ctrl(vgpu, &completion);
            break;
        case VGPU_RENDERER_DONE_VIRGL_RESOURCE:
            if (completion.has_ctrl_completion)
                (void) virtio_gpu_complete_renderer_ctrl(vgpu, &completion);
            else
                virtio_gpu_apply_renderer_side_effect(vgpu, &completion);
            break;
        case VGPU_RENDERER_DONE_FENCE:
            if (completion.has_ctrl_completion)
                (void) virtio_gpu_complete_renderer_ctrl(vgpu, &completion);
            break;
        case VGPU_RENDERER_DONE_FATAL:
            virtio_gpu_set_fail(vgpu);
            break;
        }
        virtio_gpu_release_renderer_completion(&completion);
    }
#else
    (void) vgpu;
#endif
}

void virtio_gpu_cmd_undefined_handler(virtio_gpu_state_t *vgpu,
                                      struct virtq_desc *vq_desc,
                                      uint32_t *plen)
{
    struct virtio_gpu_ctrl_hdr *header = virtio_gpu_get_request(
        vgpu, vq_desc, sizeof(struct virtio_gpu_ctrl_hdr));
    if (!header) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    fprintf(stderr,
            VIRTIO_GPU_LOG_PREFIX
            "%s(): unsupported VirtIO-GPU command type "
            "%u\n",
            __func__, header->type);

    virtio_gpu_set_fail(vgpu);
    *plen = 0;
}

static bool virtio_gpu_queue_available(virtio_gpu_state_t *vgpu,
                                       const struct virtq *queue,
                                       uint16_t *available)
{
    uint16_t avail_idx;
    uint16_t delta;

    if (!queue || !queue->ready || !available)
        return false;

    if (!ram_dma_read(vgpu->common.dma, queue->driver_addr + 2, &avail_idx,
                      sizeof(avail_idx))) {
        virtio_gpu_set_fail(vgpu);
        return false;
    }

    delta = (uint16_t) (avail_idx - queue->last_avail);
    if (delta > queue->queue_size) {
        fprintf(stderr,
                VIRTIO_GPU_LOG_PREFIX
                "%s(): avail index advanced by %u entries, exceeds queue "
                "size %u\n",
                __func__, (unsigned) delta, (unsigned) queue->queue_size);
        virtio_gpu_set_fail(vgpu);
        return false;
    }

    *available = delta;
    return true;
}

static int virtio_gpu_append_iov_desc(struct virtq_desc *vq_desc,
                                      size_t capacity,
                                      size_t total,
                                      size_t *count,
                                      const struct virtq_iov *iov,
                                      bool writable)
{
    uint16_t flags = writable ? VIRTIO_DESC_F_WRITE : 0;

    if (*count >= capacity || !iov)
        return -1;
    if (iov->addr > UINT32_MAX)
        return -1;
    if (*count + 1 < total)
        flags |= VIRTIO_DESC_F_NEXT;

    vq_desc[*count] = (struct virtq_desc) {
        .addr = iov->addr,
        .len = iov->len,
        .flags = flags,
        .next = *count + 1 < total ? (uint16_t) (*count + 1) : 0,
    };
    (*count)++;
    return 0;
}

static int virtio_gpu_chain_to_descs(const struct virtq_chain *chain,
                                     struct virtq_desc *vq_desc,
                                     size_t capacity)
{
    size_t count = 0;
    size_t total;

    if (!chain || !vq_desc || chain->readable_count == 0)
        return -1;
    total = chain->readable_count + chain->writable_count;
    if (total == 0 || total > capacity)
        return -1;

    for (size_t i = 0; i < chain->readable_count; i++) {
        if (virtio_gpu_append_iov_desc(vq_desc, capacity, total, &count,
                                       &chain->readable[i], false) < 0)
            return -1;
    }
    for (size_t i = 0; i < chain->writable_count; i++) {
        if (virtio_gpu_append_iov_desc(vq_desc, capacity, total, &count,
                                       &chain->writable[i], true) < 0)
            return -1;
    }

    return 0;
}

static bool virtio_gpu_command_requires_virgl(uint32_t type)
{
    switch (type) {
    case VIRTIO_GPU_CMD_GET_CAPSET_INFO:
    case VIRTIO_GPU_CMD_GET_CAPSET:
    case VIRTIO_GPU_CMD_RESOURCE_ASSIGN_UUID:
    case VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB:
    case VIRTIO_GPU_CMD_CTX_CREATE:
    case VIRTIO_GPU_CMD_CTX_DESTROY:
    case VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE:
    case VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE:
    case VIRTIO_GPU_CMD_RESOURCE_CREATE_3D:
    case VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D:
    case VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D:
    case VIRTIO_GPU_CMD_SUBMIT_3D:
    case VIRTIO_GPU_CMD_RESOURCE_MAP_BLOB:
    case VIRTIO_GPU_CMD_RESOURCE_UNMAP_BLOB:
        return true;
    default:
        return false;
    }
}

static int virtio_gpu_desc_handler(virtio_gpu_state_t *vgpu,
                                   int queue_index,
                                   const struct virtq_chain *chain,
                                   uint32_t *plen)
{
    struct virtq_desc vq_desc[VIRTIO_GPU_MAX_DESC] = {0};

    if (virtio_gpu_chain_to_descs(chain, vq_desc, ARRAY_SIZE(vq_desc)) < 0) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return -1;
    }

    struct virtio_gpu_ctrl_hdr *header = virtio_gpu_get_request(
        vgpu, vq_desc, sizeof(struct virtio_gpu_ctrl_hdr));
    if (!header) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return -1;
    }

    bool is_cursor_cmd = header->type == VIRTIO_GPU_CMD_UPDATE_CURSOR ||
                         header->type == VIRTIO_GPU_CMD_MOVE_CURSOR;
    if ((queue_index == VIRTIO_GPU_CONTROLQ && is_cursor_cmd) ||
        (queue_index == VIRTIO_GPU_CURSORQ && !is_cursor_cmd)) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return -1;
    }

    if (virtio_gpu_command_requires_virgl(header->type) &&
        !virtio_gpu_virgl_runtime_ready()) {
        virtio_gpu_cmd_undefined_handler(vgpu, vq_desc, plen);
        return *plen == 0 ? -1 : 0;
    }

    /* Process the command */
    switch (header->type) {
        /* 2D commands */
        VIRTIO_GPU_CMD_CASE(GET_DISPLAY_INFO, get_display_info)
        VIRTIO_GPU_CMD_CASE(RESOURCE_CREATE_2D, resource_create_2d)
        VIRTIO_GPU_CMD_CASE(RESOURCE_UNREF, resource_unref)
        VIRTIO_GPU_CMD_CASE(SET_SCANOUT, set_scanout)
        VIRTIO_GPU_CMD_CASE(RESOURCE_FLUSH, resource_flush)
        VIRTIO_GPU_CMD_CASE(TRANSFER_TO_HOST_2D, transfer_to_host_2d)
        VIRTIO_GPU_CMD_CASE(RESOURCE_ATTACH_BACKING, resource_attach_backing)
        VIRTIO_GPU_CMD_CASE(RESOURCE_DETACH_BACKING, resource_detach_backing)
        VIRTIO_GPU_CMD_CASE(GET_CAPSET_INFO, get_capset_info)
        VIRTIO_GPU_CMD_CASE(GET_CAPSET, get_capset)
        VIRTIO_GPU_CMD_CASE(GET_EDID, get_edid)
        VIRTIO_GPU_CMD_CASE(RESOURCE_ASSIGN_UUID, resource_assign_uuid)
        VIRTIO_GPU_CMD_CASE(RESOURCE_CREATE_BLOB, resource_create_blob)
        /* 3D commands */
        VIRTIO_GPU_CMD_CASE(CTX_CREATE, ctx_create)
        VIRTIO_GPU_CMD_CASE(CTX_DESTROY, ctx_destroy)
        VIRTIO_GPU_CMD_CASE(CTX_ATTACH_RESOURCE, ctx_attach_resource)
        VIRTIO_GPU_CMD_CASE(CTX_DETACH_RESOURCE, ctx_detach_resource)
        VIRTIO_GPU_CMD_CASE(RESOURCE_CREATE_3D, resource_create_3d)
        VIRTIO_GPU_CMD_CASE(TRANSFER_TO_HOST_3D, transfer_to_host_3d)
        VIRTIO_GPU_CMD_CASE(TRANSFER_FROM_HOST_3D, transfer_from_host_3d)
        VIRTIO_GPU_CMD_CASE(SUBMIT_3D, submit_3d)
        VIRTIO_GPU_CMD_CASE(RESOURCE_MAP_BLOB, resource_map_blob)
        VIRTIO_GPU_CMD_CASE(RESOURCE_UNMAP_BLOB, resource_unmap_blob)
        VIRTIO_GPU_CMD_CASE(UPDATE_CURSOR, update_cursor)
        VIRTIO_GPU_CMD_CASE(MOVE_CURSOR, move_cursor)
    default:
        virtio_gpu_cmd_undefined_handler(vgpu, vq_desc, plen);
        return -1;
    }

    return 0;
}

static bool virtio_gpu_actor_generation_current(struct virtio_actor *actor,
                                                uint64_t generation)
{
    return actor && virtio_actor_generation(actor) == generation;
}

static bool virtio_gpu_queue_ready_for_actor(virtio_gpu_state_t *vgpu,
                                             struct virtq *queue)
{
    unsigned status = virtio_gpu_status_load(vgpu);

    if (status & VIRTIO_STATUS__DEVICE_NEEDS_RESET)
        return false;
    if ((status & VIRTIO_STATUS__DRIVER_OK) && queue && queue->ready)
        return true;

    virtio_gpu_set_fail(vgpu);
    return false;
}

static int virtio_gpu_actor_drain_queue(void *opaque,
                                        struct virtio_actor *actor,
                                        uint16_t queue_index,
                                        uint64_t generation)
{
    virtio_gpu_state_t *vgpu = opaque;
    struct virtq *queue;
    struct virtq_iov readable[VIRTIO_GPU_QUEUE_NUM_MAX];
    struct virtq_iov writable[VIRTIO_GPU_QUEUE_NUM_MAX];
    bool consumed = false;

    if (!vgpu || queue_index >= vgpu->common.num_queues) {
        if (vgpu)
            virtio_gpu_set_fail(vgpu);
        return 0;
    }

    queue = &vgpu->common.queues[queue_index];
    vgpu->actor_drain_generation = generation;

    if (!virtio_gpu_actor_generation_current(actor, generation))
        return 0;
    if (!virtio_gpu_queue_ready_for_actor(vgpu, queue))
        return 0;

    for (;;) {
        struct virtq_chain chain = {
            .readable = readable,
            .readable_capacity = ARRAY_SIZE(readable),
            .writable = writable,
            .writable_capacity = ARRAY_SIZE(writable),
        };
        uint16_t available;
        uint32_t len = 0;
        int ret;

        if (!virtio_gpu_actor_generation_current(actor, generation))
            return 0;
        if (!virtio_gpu_queue_available(vgpu, queue, &available))
            return 0;
        if (available == 0)
            break;

        ret = virtq_pop(vgpu->common.dma, queue, &chain);
        if (ret < 0) {
            virtio_gpu_set_fail(vgpu);
            return 0;
        }
        if (ret == 0)
            break;

        if (!virtio_gpu_actor_generation_current(actor, generation))
            return 0;

        vgpu->ctrl_dispatch = (struct virtio_gpu_ctrl_dispatch_context) {
            .active = true,
            .queue_index = queue_index,
            .desc_head = chain.head,
            .actor_generation = generation,
            .common_generation = vgpu->common.generation,
            .trigger_irq = true,
        };
        ret = virtio_gpu_desc_handler(vgpu, queue_index, &chain, &len);
        vgpu->ctrl_dispatch = (struct virtio_gpu_ctrl_dispatch_context) {0};
        if (ret != 0)
            return 0;
        if (len == VIRTIO_GPU_RESPONSE_DEFERRED)
            continue;

        if (!virtio_actor_begin_completion(actor, generation))
            return 0;
        ret = virtq_add_used(vgpu->common.dma, queue, chain.head, len);
        virtio_actor_end_completion(actor);
        if (ret < 0) {
            virtio_gpu_set_fail(vgpu);
            return 0;
        }
        consumed = true;

        if (virtio_gpu_status_load(vgpu) & VIRTIO_STATUS__DEVICE_NEEDS_RESET)
            break;
    }

    if (consumed && virtio_actor_begin_completion(actor, generation)) {
        if (!virtq_interrupt_suppressed(vgpu->common.dma, queue))
            virtio_irq_trigger(&vgpu->common.irq, VIRTIO_INT__USED_RING);
        virtio_actor_end_completion(actor);
    }
    return 0;
}

static bool virtio_gpu_actor_queue_has_work(void *opaque,
                                            struct virtio_actor *actor,
                                            uint16_t queue_index,
                                            uint64_t generation)
{
    virtio_gpu_state_t *vgpu = opaque;
    uint16_t available = 0;

    if (!vgpu || queue_index >= vgpu->common.num_queues)
        return false;
    if (!virtio_gpu_actor_generation_current(actor, generation))
        return false;
    if (!virtio_gpu_queue_ready_for_actor(vgpu,
                                          &vgpu->common.queues[queue_index]))
        return false;
    if (!virtio_gpu_queue_available(vgpu, &vgpu->common.queues[queue_index],
                                    &available))
        return false;
    return available != 0;
}

static const struct virtio_actor_ops virtio_gpu_actor_ops = {
    .drain_queue = virtio_gpu_actor_drain_queue,
    .queue_has_work = virtio_gpu_actor_queue_has_work,
};

static bool virtio_gpu_config_range_valid(uint32_t offset, uint32_t size)
{
    return size != 0 && offset < sizeof(struct virtio_gpu_config) &&
           size <= sizeof(struct virtio_gpu_config) - offset;
}

static inline bool virtio_gpu_is_config_access(uint32_t addr,
                                               size_t access_size)
{
    const uint32_t base = VIRTIO_Config << 2;
    const uint32_t end = base + (uint32_t) sizeof(struct virtio_gpu_config);

    if (access_size == 0 || addr < base || addr >= end)
        return false;
    return access_size <= end - addr;
}

static uint32_t virtio_gpu_read_config(void *opaque,
                                       uint32_t offset,
                                       uint32_t size)
{
    virtio_gpu_state_t *vgpu = opaque;
    struct virtio_gpu_config config = {
        .events_read = 0,
        .events_clear = 0,
        .num_scanouts = PRIV(vgpu)->num_scanouts,
        .num_capsets =
            virtio_gpu_virgl_runtime_ready() ? PRIV(vgpu)->num_capsets : 0,
    };
    uint32_t value = 0;

    if (!virtio_gpu_config_range_valid(offset, size))
        return 0;

    memcpy(&value, (uint8_t *) &config + offset, size);
    return value;
}

static void virtio_gpu_write_config(void *opaque,
                                    uint32_t offset,
                                    uint32_t size,
                                    uint32_t value)
{
    (void) opaque;
    (void) offset;
    (void) size;
    (void) value;
    /* No display events are currently implemented, so events_clear is a no-op.
     */
}

static bool virtio_gpu_load_width_bytes(uint8_t width, size_t *access_size)
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

static bool virtio_gpu_store_width_bytes(uint8_t width, size_t *access_size)
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

static bool virtio_gpu_config_write_allowed(uint32_t addr, size_t size)
{
    uint32_t offset = addr - (VIRTIO_Config << 2);
    uint32_t field = offsetof(struct virtio_gpu_config, events_clear);

    return virtio_gpu_config_range_valid(offset, (uint32_t) size) &&
           offset >= field && offset < field + sizeof(uint32_t) &&
           size <= field + sizeof(uint32_t) - offset;
}

void virtio_gpu_read(hart_t *vm,
                     virtio_gpu_state_t *vgpu,
                     uint32_t addr,
                     uint8_t width,
                     uint32_t *value)
{
    size_t access_size = 0;
    bool is_cfg;
    int ret;

    if (!virtio_gpu_load_width_bytes(width, &access_size)) {
        vm_set_exception(vm, RV_EXC_ILLEGAL_INSN, 0);
        return;
    }

    is_cfg = virtio_gpu_is_config_access(addr, access_size);
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

    ret = virtio_mmio_read(&vgpu->common, addr, (uint8_t) access_size, value);
    if (ret < 0)
        vm_set_exception(vm, RV_EXC_LOAD_FAULT, vm->exc_val);
}

void virtio_gpu_write(hart_t *vm,
                      virtio_gpu_state_t *vgpu,
                      uint32_t addr,
                      uint8_t width,
                      uint32_t value)
{
    size_t access_size = 0;
    bool is_cfg;
    int ret;

    if (!virtio_gpu_store_width_bytes(width, &access_size)) {
        vm_set_exception(vm, RV_EXC_ILLEGAL_INSN, 0);
        return;
    }

    is_cfg = virtio_gpu_is_config_access(addr, access_size);
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
        if (!virtio_gpu_config_write_allowed(addr, access_size)) {
            vm_set_exception(vm, RV_EXC_STORE_FAULT, vm->exc_val);
            return;
        }
    }

    ret = virtio_mmio_write(&vgpu->common, addr, (uint8_t) access_size, value);
    if (ret < 0)
        vm_set_exception(vm, RV_EXC_STORE_FAULT, vm->exc_val);
}

bool virtio_gpu_irq_pending(virtio_gpu_state_t *vgpu)
{
    return virtio_irq_read_status(&vgpu->common.irq) != 0;
}

static int virtio_gpu_activate(void *opaque,
                               const struct virtio_activation_context *ctx)
{
    virtio_gpu_state_t *vgpu = opaque;
    int ret;

    (void) ctx;

    ret = virtio_actor_start(&vgpu->actor);
    if (ret < 0 && ret != -EALREADY)
        return ret;

    ret = virtio_actor_enter_configuring(&vgpu->actor);
    if (ret < 0)
        return ret;
    return virtio_actor_activate(&vgpu->actor);
}

static int virtio_gpu_prepare_reset(void *opaque,
                                    uint64_t old_generation,
                                    uint64_t new_generation)
{
    virtio_gpu_state_t *vgpu = opaque;

    (void) old_generation;
    (void) new_generation;

    return virtio_actor_reset(&vgpu->actor);
}

static int virtio_gpu_reset(void *opaque,
                            uint64_t old_generation,
                            uint64_t new_generation)
{
    virtio_gpu_state_t *vgpu = opaque;

    (void) old_generation;

#if SEMU_HAS(VIRGL)
    int renderer_reset_ret;

    virtio_gpu_virgl_clear_resources();
    vgpu_renderer_reset_queues(new_generation);
    renderer_reset_ret = virtio_gpu_submit_renderer_reset(new_generation);
#else
    int renderer_reset_ret = 0;

    (void) new_generation;
#endif

    if (g_virtio_gpu_backend.reset)
        g_virtio_gpu_backend.reset(vgpu);
    return renderer_reset_ret;
}

static int virtio_gpu_notify_queue(void *opaque,
                                   uint16_t queue_index,
                                   uint64_t generation)
{
    virtio_gpu_state_t *vgpu = opaque;
    int ret;

    (void) generation;

    if (queue_index != VIRTIO_GPU_CONTROLQ &&
        queue_index != VIRTIO_GPU_CURSORQ) {
        virtio_gpu_set_fail(vgpu);
        return -EINVAL;
    }

    ret = virtio_actor_notify_queue(&vgpu->actor, queue_index);
    if (ret == -EAGAIN)
        return 0;
    if (ret < 0) {
        virtio_gpu_set_fail(vgpu);
        return ret;
    }
    return 0;
}

static const struct virtio_device_ops virtio_gpu_ops = {
    .activate = virtio_gpu_activate,
    .prepare_reset = virtio_gpu_prepare_reset,
    .reset = virtio_gpu_reset,
    .notify_queue = virtio_gpu_notify_queue,
    .read_config = virtio_gpu_read_config,
    .write_config = virtio_gpu_write_config,
};

void virtio_gpu_init(virtio_gpu_state_t *vgpu, emu_state_t *emu)
{
    static const uint16_t queue_max_sizes[] = {
        [VIRTIO_GPU_CONTROLQ] = VIRTIO_GPU_QUEUE_NUM_MAX,
        [VIRTIO_GPU_CURSORQ] = VIRTIO_GPU_QUEUE_NUM_MAX,
    };
    struct virtio_device_common_config config;
#if SEMU_HAS(VIRGL)
    const struct virtio_device_shm_region host_visible_shm = {
        .id = VIRTIO_GPU_SHM_ID_HOST_VISIBLE,
        .base = SEMU_PLATFORM_MMIO_VGPU_HOSTMEM_BASE,
        .length = SEMU_PLATFORM_VGPU_HOSTMEM_SIZE,
    };
#endif

    if (virtio_gpu_instance_initialized) {
        fprintf(stderr,
                VIRTIO_GPU_LOG_PREFIX
                "%s(): only one virtio-gpu instance is supported\n",
                __func__);
        exit(EXIT_FAILURE);
    }

    memset(vgpu, 0, sizeof(*vgpu));
    memset(&virtio_gpu_data, 0, sizeof(virtio_gpu_data));
    vgpu->ram = emu->ram;
    vgpu->priv = &virtio_gpu_data;
    virtio_gpu_sw_backend_init(vgpu);

    config = (struct virtio_device_common_config) {
        .emu = emu,
        .dma = &emu->ram_dma,
        .irq_source = SEMU_IRQ_SOURCE_VGPU,
        .device_id = 16,
        .vendor_id = VIRTIO_VENDOR_ID,
        .device_features = virtio_gpu_device_features(),
        .required_features = VIRTIO_GPU_F_VERSION_1,
        .queue_max_sizes = queue_max_sizes,
        .num_queues = ARRAY_SIZE(queue_max_sizes),
        .ops = &virtio_gpu_ops,
        .opaque = vgpu,
#if SEMU_HAS(VIRGL)
        .shm_region =
            virtio_gpu_virgl_runtime_ready() ? &host_visible_shm : NULL,
#endif
    };

    if (virtio_device_common_init(&vgpu->common, &config) < 0) {
        fprintf(stderr,
                VIRTIO_GPU_LOG_PREFIX
                "%s(): failed to initialize common VirtIO transport\n",
                __func__);
        exit(EXIT_FAILURE);
    }
#if SEMU_HAS(VIRGL)
    vgpu_renderer_reset_queues(vgpu->common.generation);
#endif

    if (virtio_actor_init(&vgpu->actor, &virtio_gpu_actor_ops, vgpu,
                          ARRAY_SIZE(queue_max_sizes)) < 0) {
        virtio_device_common_destroy(&vgpu->common);
        fprintf(stderr,
                VIRTIO_GPU_LOG_PREFIX
                "%s(): failed to initialize VirtIO actor\n",
                __func__);
        exit(EXIT_FAILURE);
    }
    vgpu->actor_initialized = true;

    virtio_gpu_instance_initialized = true;
}

void virtio_gpu_destroy(virtio_gpu_state_t *vgpu)
{
    if (!vgpu)
        return;

    if (vgpu->actor_initialized) {
        virtio_actor_stop(&vgpu->actor);
        virtio_actor_destroy(&vgpu->actor);
        vgpu->actor_initialized = false;
    }

    if (vgpu->priv == &virtio_gpu_data && g_virtio_gpu_backend.reset)
        g_virtio_gpu_backend.reset(vgpu);
#if SEMU_HAS(VIRGL)
    virtio_gpu_virgl_clear_resources();
    vgpu_renderer_reset_queues(vgpu->common.generation);
#endif

    virtio_device_common_destroy(&vgpu->common);

    if (vgpu->priv == &virtio_gpu_data) {
        memset(&virtio_gpu_data, 0, sizeof(virtio_gpu_data));
        vgpu->priv = NULL;
        virtio_gpu_instance_initialized = false;
    }
}

uint32_t virtio_gpu_register_scanout(virtio_gpu_state_t *vgpu,
                                     uint32_t width,
                                     uint32_t height)
{
    int scanout_num = PRIV(vgpu)->num_scanouts;
    if (scanout_num >= VIRTIO_GPU_MAX_SCANOUTS) {
        /* Registration is init-only today. Return an error instead if scanout
         * creation becomes dynamic or guest-triggered.
         */
        fprintf(stderr,
                VIRTIO_GPU_LOG_PREFIX "%s(): exceeded scanout maximum number\n",
                __func__);
        exit(EXIT_FAILURE);
    }

    PRIV(vgpu)->scanouts[scanout_num].width = width;
    PRIV(vgpu)->scanouts[scanout_num].height = height;
    PRIV(vgpu)->scanouts[scanout_num].enabled = 1;
    PRIV(vgpu)->scanouts[scanout_num].primary_resource_id = 0;
    PRIV(vgpu)->scanouts[scanout_num].cursor_resource_id = 0;
    PRIV(vgpu)->scanouts[scanout_num].src_x = 0;
    PRIV(vgpu)->scanouts[scanout_num].src_y = 0;
    PRIV(vgpu)->scanouts[scanout_num].src_w = 0;
    PRIV(vgpu)->scanouts[scanout_num].src_h = 0;

    /* 'scanout_num' will match the guest-visible 'scanout_id'. See
     * 'virtio_gpu_get_display_info_handler()' above for how that index is
     * exposed to the guest and later reused in 'SET_SCANOUT'/'GET_EDID'.
     */
    PRIV(vgpu)->num_scanouts++;

    return (uint32_t) scanout_num;
}
