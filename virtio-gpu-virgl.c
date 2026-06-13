#include "virtio-gpu-virgl.h"

#include "platform.h"
#include "vgpu-display.h"

#include <limits.h>
#include <pthread.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>

#include <virglrenderer.h>

#define VGPU_VIRGL_PENDING_FENCES_MAX VGPU_RENDERER_QUEUE_CAPACITY

struct vgpu_virgl_pending_fence {
    bool active;
    bool context_fence;
    uint32_t ctx_id;
    uint32_t ring_idx;
    uint64_t generation;
    uint64_t renderer_fence_id;
    uint64_t guest_fence_id;
};

static pthread_mutex_t vgpu_virgl_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t vgpu_virgl_resource_lock = PTHREAD_MUTEX_INITIALIZER;
static struct vgpu_virgl_pending_fence
    vgpu_virgl_pending_fences[VGPU_VIRGL_PENDING_FENCES_MAX];
static uint32_t vgpu_virgl_pending_fence_count;
static bool vgpu_virgl_poll_request_pending;
static uint32_t vgpu_virgl_next_ctx0_renderer_fence;
static uint64_t vgpu_virgl_next_context_renderer_fence;
static uint64_t debug_poll_requests_submitted;
static uint64_t debug_poll_requests_dropped;
static uint64_t debug_poll_requests_executed;
static uint64_t debug_fences_created;
static uint64_t debug_fences_completed;
static uint64_t debug_last_ctx0_fence;
static uint32_t debug_last_context_ctx_id;
static uint32_t debug_last_context_ring_idx;
static uint64_t debug_last_context_fence;

struct vgpu_virgl_renderer_resource {
    uint32_t resource_id;
    bool blob_resource;
    bool backing_attached;
    bool mapped;
    uint64_t blob_size;
    uint64_t map_offset;
    void *map_ptr;
    uint64_t map_size;
    struct vgpu_virgl_renderer_resource *next;
};

struct vgpu_virgl_renderer_scanout {
    bool active;
    uint32_t resource_id;
    uint64_t scanout_generation;
    struct virtio_gpu_rect rect;
};

struct vgpu_virgl_box {
    uint32_t x;
    uint32_t y;
    uint32_t z;
    uint32_t w;
    uint32_t h;
    uint32_t d;
};

static struct vgpu_virgl_renderer_resource *vgpu_virgl_renderer_resources;
static struct vgpu_virgl_renderer_scanout
    vgpu_virgl_renderer_scanouts[VIRTIO_GPU_MAX_SCANOUTS];

static struct vgpu_virgl_box vgpu_virgl_box_from_virtio(
    const struct virtio_gpu_box *box)
{
    return (struct vgpu_virgl_box) {
        .x = box->x,
        .y = box->y,
        .z = box->z,
        .w = box->w,
        .h = box->h,
        .d = box->d,
    };
}

static void vgpu_virgl_detach_iov(uint32_t resource_id);

static void vgpu_virgl_clear_renderer_resource_map(
    struct vgpu_virgl_renderer_resource *res)
{
    if (!res)
        return;

    res->mapped = false;
    res->map_offset = 0;
    res->map_ptr = NULL;
    res->map_size = 0;
}

static void vgpu_virgl_release_renderer_resource(
    struct vgpu_virgl_renderer_resource *res)
{
    if (!res)
        return;
    if (res->mapped) {
        (void) virgl_renderer_resource_unmap(res->resource_id);
        vgpu_virgl_clear_renderer_resource_map(res);
    }
    if (res->backing_attached)
        vgpu_virgl_detach_iov(res->resource_id);
    free(res);
}

static void vgpu_virgl_clear_renderer_resources(void)
{
    pthread_mutex_lock(&vgpu_virgl_resource_lock);
    while (vgpu_virgl_renderer_resources) {
        struct vgpu_virgl_renderer_resource *res =
            vgpu_virgl_renderer_resources;
        vgpu_virgl_renderer_resources = res->next;
        vgpu_virgl_release_renderer_resource(res);
    }
    pthread_mutex_unlock(&vgpu_virgl_resource_lock);
}

static bool vgpu_virgl_insert_renderer_resource(uint32_t resource_id,
                                                bool blob_resource,
                                                uint64_t blob_size)
{
    struct vgpu_virgl_renderer_resource *res = calloc(1, sizeof(*res));

    if (!res)
        return false;

    res->resource_id = resource_id;
    res->blob_resource = blob_resource;
    res->blob_size = blob_size;
    pthread_mutex_lock(&vgpu_virgl_resource_lock);
    res->next = vgpu_virgl_renderer_resources;
    vgpu_virgl_renderer_resources = res;
    pthread_mutex_unlock(&vgpu_virgl_resource_lock);
    return true;
}

static struct vgpu_virgl_renderer_resource *vgpu_virgl_find_renderer_resource(
    uint32_t resource_id)
{
    for (struct vgpu_virgl_renderer_resource *res =
             vgpu_virgl_renderer_resources;
         res; res = res->next) {
        if (res->resource_id == resource_id)
            return res;
    }

    return NULL;
}

static void vgpu_virgl_detach_iov(uint32_t resource_id)
{
    struct iovec *iov = NULL;
    int num_iovs = 0;

    virgl_renderer_resource_detach_iov(resource_id, &iov, &num_iovs);
    (void) num_iovs;
    free(iov);
}

static bool vgpu_virgl_remove_renderer_resource(uint32_t resource_id)
{
    struct vgpu_virgl_renderer_resource **cursor =
        &vgpu_virgl_renderer_resources;

    pthread_mutex_lock(&vgpu_virgl_resource_lock);
    while (*cursor) {
        struct vgpu_virgl_renderer_resource *res = *cursor;

        if (res->resource_id == resource_id) {
            *cursor = res->next;
            vgpu_virgl_release_renderer_resource(res);
            pthread_mutex_unlock(&vgpu_virgl_resource_lock);
            return true;
        }
        cursor = &res->next;
    }

    pthread_mutex_unlock(&vgpu_virgl_resource_lock);
    return false;
}

static bool vgpu_virgl_hostmem_width_ok(uint8_t width)
{
    return width == 1 || width == 2 || width == 4;
}

static bool vgpu_virgl_hostmem_range_valid(uint64_t offset, uint64_t size)
{
    if (size == 0 || size > SEMU_PLATFORM_VGPU_HOSTMEM_SIZE)
        return false;
    if (offset > UINT64_MAX - size)
        return false;
    return offset <= SEMU_PLATFORM_VGPU_HOSTMEM_SIZE - size;
}

static bool vgpu_virgl_find_hostmem_mapping_locked(uint64_t off,
                                                   uint8_t width,
                                                   uint8_t **ptr)
{
    uint64_t end;

    if (!ptr || !vgpu_virgl_hostmem_width_ok(width))
        return false;
    if (off > UINT64_MAX - width)
        return false;
    end = off + width;

    for (struct vgpu_virgl_renderer_resource *res =
             vgpu_virgl_renderer_resources;
         res; res = res->next) {
        uint64_t map_end;

        if (!res->blob_resource || !res->mapped || !res->map_ptr)
            continue;
        if (res->map_size < res->blob_size)
            continue;
        if (res->map_offset > UINT64_MAX - res->blob_size)
            continue;
        map_end = res->map_offset + res->blob_size;
        if (off < res->map_offset || end > map_end)
            continue;

        *ptr = (uint8_t *) res->map_ptr + (off - res->map_offset);
        return true;
    }

    return false;
}

bool vgpu_virgl_hostmem_read(uint64_t off, uint8_t width, uint32_t *value)
{
    uint8_t *ptr = NULL;
    uint32_t result = 0;

    if (!value)
        return false;

    pthread_mutex_lock(&vgpu_virgl_resource_lock);
    if (!vgpu_virgl_find_hostmem_mapping_locked(off, width, &ptr)) {
        pthread_mutex_unlock(&vgpu_virgl_resource_lock);
        return false;
    }

    for (uint8_t i = 0; i < width; i++)
        result |= (uint32_t) ptr[i] << (i * 8);
    pthread_mutex_unlock(&vgpu_virgl_resource_lock);

    *value = result;
    return true;
}

bool vgpu_virgl_hostmem_write(uint64_t off, uint8_t width, uint32_t value)
{
    uint8_t *ptr = NULL;

    pthread_mutex_lock(&vgpu_virgl_resource_lock);
    if (!vgpu_virgl_find_hostmem_mapping_locked(off, width, &ptr)) {
        pthread_mutex_unlock(&vgpu_virgl_resource_lock);
        return false;
    }

    for (uint8_t i = 0; i < width; i++)
        ptr[i] = (uint8_t) (value >> (i * 8));
    pthread_mutex_unlock(&vgpu_virgl_resource_lock);
    return true;
}

static void vgpu_virgl_clear_renderer_resource_scanouts(uint32_t resource_id)
{
    for (uint32_t i = 0; i < VIRTIO_GPU_MAX_SCANOUTS; i++) {
        if (vgpu_virgl_renderer_scanouts[i].active &&
            vgpu_virgl_renderer_scanouts[i].resource_id == resource_id)
            vgpu_virgl_renderer_scanouts[i] =
                (struct vgpu_virgl_renderer_scanout) {0};
    }
}

static bool vgpu_virgl_rect_fits_resource(
    const struct virtio_gpu_rect *rect,
    const struct virgl_renderer_resource_info *info)
{
    return rect && info && rect->width != 0 && rect->height != 0 &&
           info->width != 0 && info->height != 0 && rect->x < info->width &&
           rect->y < info->height && rect->width <= info->width - rect->x &&
           rect->height <= info->height - rect->y;
}

static int vgpu_virgl_record_renderer_scanout_view(
    struct vgpu_renderer_ctrl_payload *payload,
    uint32_t scanout_id,
    uint32_t resource_id,
    const struct virtio_gpu_rect *rect)
{
    const struct virtio_gpu_scanout_info *scanout = &payload->scanout;
    struct virgl_renderer_resource_info info = {0};

    if (scanout_id >= VIRTIO_GPU_MAX_SCANOUTS || !scanout->enabled)
        return VIRTIO_GPU_RESP_ERR_INVALID_SCANOUT_ID;
    if (!vgpu_virgl_find_renderer_resource(resource_id))
        return VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
    if (!rect || rect->width == 0 || rect->height == 0 ||
        rect->width > scanout->width || rect->height > scanout->height)
        return VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;

    if (virgl_renderer_resource_get_info((int) resource_id, &info) != 0 ||
        info.tex_id == 0)
        return VIRTIO_GPU_RESP_ERR_UNSPEC;
    if (!vgpu_virgl_rect_fits_resource(rect, &info))
        return VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;

    payload->has_gl_scanout_payload = true;
    payload->gl_scanout_payload = (struct vgpu_display_gl_payload) {
        .texture_id = info.tex_id,
        .width = info.width,
        .height = info.height,
        .src_x = rect->x,
        .src_y = rect->y,
        .src_width = rect->width,
        .src_height = rect->height,
        .y_0_top = false,
    };

    vgpu_virgl_renderer_scanouts[scanout_id] =
        (struct vgpu_virgl_renderer_scanout) {
            .active = true,
            .resource_id = resource_id,
            .scanout_generation = payload->scanout_generation,
            .rect = *rect,
        };
    return VIRTIO_GPU_RESP_OK_NODATA;
}

static int vgpu_virgl_record_renderer_scanout(
    struct vgpu_renderer_ctrl_payload *payload)
{
    const struct virtio_gpu_set_scanout *cmd = &payload->cmd.set_scanout;

    return vgpu_virgl_record_renderer_scanout_view(payload, cmd->scanout_id,
                                                   cmd->resource_id, &cmd->r);
}

static int vgpu_virgl_record_renderer_scanout_blob(
    struct vgpu_renderer_ctrl_payload *payload)
{
    const struct virtio_gpu_set_scanout_blob *cmd =
        &payload->cmd.set_scanout_blob;

    return vgpu_virgl_record_renderer_scanout_view(payload, cmd->scanout_id,
                                                   cmd->resource_id, &cmd->r);
}

static bool vgpu_virgl_fence_stream_matches(
    const struct vgpu_virgl_pending_fence *pending,
    bool context_fence,
    uint32_t ctx_id,
    uint32_t ring_idx)
{
    if (pending->context_fence != context_fence)
        return false;
    if (!context_fence)
        return true;

    return pending->ctx_id == ctx_id && pending->ring_idx == ring_idx;
}

static uint32_t vgpu_virgl_alloc_ctx0_renderer_fence_locked(void)
{
    vgpu_virgl_next_ctx0_renderer_fence++;
    if (!vgpu_virgl_next_ctx0_renderer_fence)
        vgpu_virgl_next_ctx0_renderer_fence++;

    return vgpu_virgl_next_ctx0_renderer_fence;
}

static uint64_t vgpu_virgl_alloc_context_renderer_fence_locked(void)
{
    vgpu_virgl_next_context_renderer_fence++;
    if (!vgpu_virgl_next_context_renderer_fence)
        vgpu_virgl_next_context_renderer_fence++;

    return vgpu_virgl_next_context_renderer_fence;
}

static bool vgpu_virgl_record_pending_fence(uint64_t generation,
                                            bool context_fence,
                                            uint32_t ctx_id,
                                            uint32_t ring_idx,
                                            uint64_t renderer_fence_id,
                                            uint64_t guest_fence_id)
{
    for (size_t i = 0; i < VGPU_VIRGL_PENDING_FENCES_MAX; i++) {
        struct vgpu_virgl_pending_fence *pending =
            &vgpu_virgl_pending_fences[i];
        if (pending->active)
            continue;

        *pending = (struct vgpu_virgl_pending_fence) {
            .active = true,
            .context_fence = context_fence,
            .ctx_id = context_fence ? ctx_id : 0,
            .ring_idx = context_fence ? ring_idx : 0,
            .generation = generation,
            .renderer_fence_id = renderer_fence_id,
            .guest_fence_id = guest_fence_id,
        };
        vgpu_virgl_pending_fence_count++;
        debug_fences_created++;
        return true;
    }

    return false;
}

static bool vgpu_virgl_cancel_pending_fence(bool context_fence,
                                            uint32_t ctx_id,
                                            uint32_t ring_idx,
                                            uint64_t renderer_fence_id)
{
    bool canceled = false;

    pthread_mutex_lock(&vgpu_virgl_lock);
    for (size_t i = 0; i < VGPU_VIRGL_PENDING_FENCES_MAX; i++) {
        struct vgpu_virgl_pending_fence *pending =
            &vgpu_virgl_pending_fences[i];
        if (!pending->active ||
            !vgpu_virgl_fence_stream_matches(pending, context_fence, ctx_id,
                                             ring_idx) ||
            pending->renderer_fence_id != renderer_fence_id)
            continue;

        pending->active = false;
        if (vgpu_virgl_pending_fence_count)
            vgpu_virgl_pending_fence_count--;
        canceled = true;
        break;
    }
    pthread_mutex_unlock(&vgpu_virgl_lock);

    return canceled;
}

static void vgpu_virgl_request_poll_locked(void)
{
    struct vgpu_renderer_request request = {
        .type = VGPU_RENDERER_REQ_POLL,
    };

    if (!vgpu_virgl_pending_fence_count || vgpu_virgl_poll_request_pending)
        return;

    vgpu_virgl_poll_request_pending = true;
    if (vgpu_renderer_submit(&request)) {
        debug_poll_requests_submitted++;
        return;
    }

    vgpu_virgl_poll_request_pending = false;
    debug_poll_requests_dropped++;
}

static bool vgpu_virgl_take_completed_fence(bool context_fence,
                                            uint32_t ctx_id,
                                            uint32_t ring_idx,
                                            uint64_t renderer_fence_id,
                                            uint64_t *generation,
                                            uint64_t *guest_fence_id)
{
    uint64_t best_renderer_fence = 0;
    uint64_t best_generation = 0;
    uint64_t best_guest_fence = 0;
    size_t best_index = 0;
    bool found = false;

    for (size_t i = 0; i < VGPU_VIRGL_PENDING_FENCES_MAX; i++) {
        const struct vgpu_virgl_pending_fence *pending =
            &vgpu_virgl_pending_fences[i];
        if (!pending->active ||
            !vgpu_virgl_fence_stream_matches(pending, context_fence, ctx_id,
                                             ring_idx) ||
            pending->renderer_fence_id > renderer_fence_id)
            continue;
        if (found && pending->renderer_fence_id < best_renderer_fence)
            continue;

        found = true;
        best_renderer_fence = pending->renderer_fence_id;
        best_generation = pending->generation;
        best_guest_fence = pending->guest_fence_id;
        best_index = i;
    }

    if (!found)
        return false;

    vgpu_virgl_pending_fences[best_index].active = false;
    if (vgpu_virgl_pending_fence_count)
        vgpu_virgl_pending_fence_count--;
    debug_fences_completed++;

    for (size_t i = 0; i < VGPU_VIRGL_PENDING_FENCES_MAX; i++) {
        struct vgpu_virgl_pending_fence *pending =
            &vgpu_virgl_pending_fences[i];
        if (!pending->active ||
            !vgpu_virgl_fence_stream_matches(pending, context_fence, ctx_id,
                                             ring_idx) ||
            pending->generation != best_generation ||
            pending->renderer_fence_id > renderer_fence_id)
            continue;

        pending->active = false;
        if (vgpu_virgl_pending_fence_count)
            vgpu_virgl_pending_fence_count--;
        debug_fences_completed++;
    }

    *generation = best_generation;
    *guest_fence_id = best_guest_fence;
    return true;
}

static void vgpu_virgl_write_fence(void *cookie, uint32_t fence)
{
    struct vgpu_renderer_completion completion = {
        .type = VGPU_RENDERER_DONE_FENCE,
    };
    uint64_t generation = 0;
    uint64_t guest_fence_id = 0;

    (void) cookie;

    pthread_mutex_lock(&vgpu_virgl_lock);
    if (!vgpu_virgl_take_completed_fence(false, 0, 0, fence, &generation,
                                         &guest_fence_id)) {
        pthread_mutex_unlock(&vgpu_virgl_lock);
        return;
    }
    debug_last_ctx0_fence = fence;
    pthread_mutex_unlock(&vgpu_virgl_lock);

    completion.token.generation = generation;
    completion.context_fence = false;
    completion.fence_id = guest_fence_id;
    vgpu_renderer_complete(&completion);
}

static void vgpu_virgl_write_context_fence(void *cookie,
                                           uint32_t ctx_id,
                                           uint32_t ring_idx,
                                           uint64_t fence_id)
{
    struct vgpu_renderer_completion completion = {
        .type = VGPU_RENDERER_DONE_FENCE,
    };
    uint64_t generation = 0;
    uint64_t guest_fence_id = 0;

    (void) cookie;

    pthread_mutex_lock(&vgpu_virgl_lock);
    if (!vgpu_virgl_take_completed_fence(true, ctx_id, ring_idx, fence_id,
                                         &generation, &guest_fence_id)) {
        pthread_mutex_unlock(&vgpu_virgl_lock);
        return;
    }
    debug_last_context_ctx_id = ctx_id;
    debug_last_context_ring_idx = ring_idx;
    debug_last_context_fence = fence_id;
    pthread_mutex_unlock(&vgpu_virgl_lock);

    completion.token.generation = generation;
    completion.context_fence = true;
    completion.ctx_id = ctx_id;
    completion.ring_idx = ring_idx;
    completion.fence_id = guest_fence_id;
    vgpu_renderer_complete(&completion);
}

static virgl_renderer_gl_context vgpu_virgl_create_context(
    void *cookie,
    int scanout_idx,
    struct virgl_renderer_gl_ctx_param *param)
{
    (void) cookie;
    return vgpu_window_virgl_create_context(scanout_idx, param);
}

static void vgpu_virgl_destroy_context(void *cookie,
                                       virgl_renderer_gl_context ctx)
{
    (void) cookie;
    vgpu_window_virgl_destroy_context(ctx);
}

static int vgpu_virgl_make_current(void *cookie,
                                   int scanout_idx,
                                   virgl_renderer_gl_context ctx)
{
    (void) cookie;
    return vgpu_window_virgl_make_current(scanout_idx, ctx);
}

static struct virgl_renderer_callbacks vgpu_virgl_callbacks = {
    .version = VIRGL_RENDERER_CALLBACKS_VERSION,
    .write_fence = vgpu_virgl_write_fence,
    .create_gl_context = vgpu_virgl_create_context,
    .destroy_gl_context = vgpu_virgl_destroy_context,
    .make_current = vgpu_virgl_make_current,
    .write_context_fence = vgpu_virgl_write_context_fence,
};

int vgpu_virgl_init_renderer(void *cookie)
{
    return virgl_renderer_init(cookie, VIRGL_RENDERER_THREAD_SYNC,
                               &vgpu_virgl_callbacks);
}

void vgpu_virgl_reset_renderer(void)
{
    pthread_mutex_lock(&vgpu_virgl_lock);
    memset(vgpu_virgl_pending_fences, 0, sizeof(vgpu_virgl_pending_fences));
    vgpu_virgl_pending_fence_count = 0;
    vgpu_virgl_poll_request_pending = false;
    debug_last_ctx0_fence = 0;
    debug_last_context_ctx_id = 0;
    debug_last_context_ring_idx = 0;
    debug_last_context_fence = 0;
    pthread_mutex_unlock(&vgpu_virgl_lock);

    vgpu_virgl_clear_renderer_resources();
    memset(vgpu_virgl_renderer_scanouts, 0,
           sizeof(vgpu_virgl_renderer_scanouts));
    virgl_renderer_reset();
}

static uint32_t vgpu_virgl_capset_id_for_index(uint32_t capset_index)
{
    uint32_t max_version = 0;
    uint32_t max_size = 0;
    uint32_t index = 0;

    virgl_renderer_get_cap_set(VIRTIO_GPU_CAPSET_VIRGL, &max_version,
                               &max_size);
    if (max_version && max_size) {
        if (capset_index == index)
            return VIRTIO_GPU_CAPSET_VIRGL;
        index++;
    }

    max_version = 0;
    max_size = 0;
    virgl_renderer_get_cap_set(VIRTIO_GPU_CAPSET_VIRGL2, &max_version,
                               &max_size);
    if (max_version && max_size && capset_index == index)
        return VIRTIO_GPU_CAPSET_VIRGL2;

    return 0;
}

static void vgpu_virgl_set_ctrl_side_effect(
    struct vgpu_renderer_completion *completion,
    const struct vgpu_renderer_ctrl_payload *payload,
    uint32_t response_type)
{
    if (!completion || !payload)
        return;

    switch (payload->hdr.type) {
    case VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB:
        if (response_type != VIRTIO_GPU_RESP_OK_NODATA) {
            completion->virgl_resource.type =
                VGPU_VIRGL_RESOURCE_SIDE_EFFECT_CREATE_3D_ROLLBACK;
            completion->virgl_resource.resource_id =
                payload->cmd.resource_create_blob.resource_id;
            completion->virgl_resource.resource_generation =
                payload->resource_generation;
        }
        break;
    case VIRTIO_GPU_CMD_RESOURCE_CREATE_3D:
        if (response_type != VIRTIO_GPU_RESP_OK_NODATA) {
            completion->virgl_resource.type =
                VGPU_VIRGL_RESOURCE_SIDE_EFFECT_CREATE_3D_ROLLBACK;
            completion->virgl_resource.resource_id =
                payload->cmd.resource_create_3d.resource_id;
            completion->virgl_resource.resource_generation =
                payload->resource_generation;
        }
        break;
    case VIRTIO_GPU_CMD_RESOURCE_UNREF:
        completion->virgl_resource.type =
            response_type == VIRTIO_GPU_RESP_OK_NODATA
                ? VGPU_VIRGL_RESOURCE_SIDE_EFFECT_UNREF
                : VGPU_VIRGL_RESOURCE_SIDE_EFFECT_UNREF_ROLLBACK;
        completion->virgl_resource.resource_id =
            payload->cmd.resource_unref.resource_id;
        completion->virgl_resource.resource_generation =
            payload->resource_generation;
        break;
    case VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING:
        completion->virgl_resource.type =
            VGPU_VIRGL_RESOURCE_SIDE_EFFECT_ATTACH_BACKING;
        completion->virgl_resource.resource_id =
            payload->cmd.resource_attach_backing.resource_id;
        completion->virgl_resource.resource_generation =
            payload->resource_generation;
        completion->virgl_resource.backing_transition_success =
            response_type == VIRTIO_GPU_RESP_OK_NODATA;
        break;
    case VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING:
        completion->virgl_resource.type =
            VGPU_VIRGL_RESOURCE_SIDE_EFFECT_DETACH_BACKING;
        completion->virgl_resource.resource_id =
            payload->cmd.resource_detach_backing.resource_id;
        completion->virgl_resource.resource_generation =
            payload->resource_generation;
        completion->virgl_resource.backing_transition_success =
            response_type == VIRTIO_GPU_RESP_OK_NODATA;
        break;
    case VIRTIO_GPU_CMD_SET_SCANOUT:
    case VIRTIO_GPU_CMD_SET_SCANOUT_BLOB: {
        const bool is_blob =
            payload->hdr.type == VIRTIO_GPU_CMD_SET_SCANOUT_BLOB;
        const uint32_t scanout_id =
            is_blob ? payload->cmd.set_scanout_blob.scanout_id
                    : payload->cmd.set_scanout.scanout_id;
        const uint32_t resource_id =
            is_blob ? payload->cmd.set_scanout_blob.resource_id
                    : payload->cmd.set_scanout.resource_id;
        const struct virtio_gpu_rect *rect =
            is_blob ? &payload->cmd.set_scanout_blob.r
                    : &payload->cmd.set_scanout.r;

        completion->virgl_resource.type =
            response_type == VIRTIO_GPU_RESP_OK_NODATA
                ? VGPU_VIRGL_RESOURCE_SIDE_EFFECT_SET_SCANOUT
                : VGPU_VIRGL_RESOURCE_SIDE_EFFECT_SET_SCANOUT_ROLLBACK;
        completion->virgl_resource.scanout_count = 1;
        completion->virgl_resource.scanouts[0] =
            (struct vgpu_virgl_scanout_side_effect) {
                .scanout_id = scanout_id,
                .scanout_generation = payload->scanout_generation,
                .resource_generation = payload->resource_generation,
                .has_gl_payload = payload->has_gl_scanout_payload,
                .gl_payload = payload->gl_scanout_payload,
                .scanout = payload->scanout,
            };
        completion->virgl_resource.scanouts[0].scanout.primary_resource_id =
            resource_id;
        completion->virgl_resource.scanouts[0].scanout.src_x = rect->x;
        completion->virgl_resource.scanouts[0].scanout.src_y = rect->y;
        completion->virgl_resource.scanouts[0].scanout.src_w = rect->width;
        completion->virgl_resource.scanouts[0].scanout.src_h = rect->height;
        break;
    }
    default:
        break;
    }
}

static void vgpu_virgl_complete_ctrl_request(
    const struct vgpu_renderer_request *request,
    const struct vgpu_renderer_ctrl_payload *payload,
    uint32_t response_type,
    void *response,
    size_t response_size)
{
    struct vgpu_renderer_completion completion = {
        .type = VGPU_RENDERER_DONE_CTRL,
        .token = request->token,
        .response_type = response_type,
        .response = response,
        .response_size = response_size,
        .release_response = free,
        .has_ctrl_completion = true,
        .ctrl_completion = payload->ctrl_completion,
        .has_response_desc = true,
        .request_hdr = payload->hdr,
        .response_desc = payload->response_desc,
    };

    vgpu_virgl_set_ctrl_side_effect(&completion, payload, response_type);
    (void) vgpu_renderer_complete(&completion);
}

static void vgpu_virgl_execute_ctrl_request(
    const struct vgpu_renderer_request *request,
    struct vgpu_renderer_ctrl_payload *payload)
{
    uint32_t response_type = payload->response_type;
    void *response = NULL;
    size_t response_size = 0;

    switch (request->command_type) {
    case VIRTIO_GPU_CMD_GET_CAPSET_INFO: {
        const struct virtio_gpu_get_capset_info *cmd =
            &payload->cmd.get_capset_info;
        struct virtio_gpu_resp_capset_info *out = calloc(1, sizeof(*out));
        if (!out) {
            response_type = VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY;
            break;
        }

        out->hdr.type = VIRTIO_GPU_RESP_OK_CAPSET_INFO;
        if (cmd->hdr.flags & VIRTIO_GPU_FLAG_FENCE) {
            out->hdr.flags = VIRTIO_GPU_FLAG_FENCE;
            out->hdr.fence_id = cmd->hdr.fence_id;
        }
        out->capset_id = vgpu_virgl_capset_id_for_index(cmd->capset_index);
        if (out->capset_id) {
            uint32_t max_version = 0;
            uint32_t max_size = 0;

            virgl_renderer_get_cap_set(out->capset_id, &max_version, &max_size);
            out->capset_max_version = max_version;
            out->capset_max_size = max_size;
        }
        response = out;
        response_size = sizeof(*out);
        response_type = VIRTIO_GPU_RESP_OK_CAPSET_INFO;
        break;
    }
    case VIRTIO_GPU_CMD_GET_CAPSET: {
        const struct virtio_gpu_get_capset *cmd = &payload->cmd.get_capset;
        uint32_t max_version = 0;
        uint32_t max_size = 0;

        virgl_renderer_get_cap_set(cmd->capset_id, &max_version, &max_size);
        if (!max_version || !max_size || cmd->capset_version > max_version) {
            response_type = VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
            break;
        }

        response_size = sizeof(struct virtio_gpu_resp_capset) + max_size;
        if (response_size < max_size || response_size > UINT32_MAX ||
            payload->response_capacity < response_size) {
            response_type = VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
            response_size = 0;
            break;
        }

        struct virtio_gpu_resp_capset *out = calloc(1, response_size);
        if (!out) {
            response_type = VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY;
            response_size = 0;
            break;
        }
        out->hdr.type = VIRTIO_GPU_RESP_OK_CAPSET;
        if (cmd->hdr.flags & VIRTIO_GPU_FLAG_FENCE) {
            out->hdr.flags = VIRTIO_GPU_FLAG_FENCE;
            out->hdr.fence_id = cmd->hdr.fence_id;
        }
        virgl_renderer_fill_caps(cmd->capset_id, cmd->capset_version,
                                 out->capset_data);
        response = out;
        response_type = VIRTIO_GPU_RESP_OK_CAPSET;
        break;
    }
    case VIRTIO_GPU_CMD_CTX_CREATE: {
        const struct virtio_gpu_ctx_create *cmd = &payload->cmd.ctx_create;
        int ret = virgl_renderer_context_create(cmd->hdr.ctx_id, cmd->nlen,
                                                cmd->debug_name);
        response_type =
            ret ? VIRTIO_GPU_RESP_ERR_UNSPEC : VIRTIO_GPU_RESP_OK_NODATA;
        break;
    }
    case VIRTIO_GPU_CMD_CTX_DESTROY:
        virgl_renderer_context_destroy(payload->cmd.ctx_destroy.hdr.ctx_id);
        response_type = VIRTIO_GPU_RESP_OK_NODATA;
        break;
    case VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB: {
        const struct virtio_gpu_resource_create_blob *cmd =
            &payload->cmd.resource_create_blob;
        struct virgl_renderer_resource_create_blob_args args = {
            .res_handle = cmd->resource_id,
            .ctx_id = cmd->hdr.ctx_id,
            .blob_mem = cmd->blob_mem,
            .blob_flags = cmd->blob_flags,
            .blob_id = cmd->blob_id,
            .size = cmd->size,
            .iovecs = payload->iov,
            .num_iovs = payload->iov_count,
        };
        int ret = virgl_renderer_resource_create_blob(&args);

        if (ret) {
            response_type = VIRTIO_GPU_RESP_ERR_UNSPEC;
            break;
        }
        if (!vgpu_virgl_insert_renderer_resource(cmd->resource_id, true,
                                                 cmd->size)) {
            virgl_renderer_resource_unref(cmd->resource_id);
            response_type = VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY;
            break;
        }
        response_type = VIRTIO_GPU_RESP_OK_NODATA;
        break;
    }
    case VIRTIO_GPU_CMD_RESOURCE_CREATE_3D: {
        const struct virtio_gpu_resource_create_3d *cmd =
            &payload->cmd.resource_create_3d;
        struct virgl_renderer_resource_create_args args = {
            .handle = cmd->resource_id,
            .target = cmd->target,
            .format = cmd->format,
            .bind = cmd->bind,
            .width = cmd->width,
            .height = cmd->height,
            .depth = cmd->depth,
            .array_size = cmd->array_size,
            .last_level = cmd->last_level,
            .nr_samples = cmd->nr_samples,
            .flags = cmd->flags,
        };
        int ret = virgl_renderer_resource_create(&args, NULL, 0);

        if (ret) {
            response_type = VIRTIO_GPU_RESP_ERR_UNSPEC;
            break;
        }
        if (!vgpu_virgl_insert_renderer_resource(cmd->resource_id, false, 0)) {
            virgl_renderer_resource_unref(cmd->resource_id);
            response_type = VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY;
            break;
        }
        response_type = VIRTIO_GPU_RESP_OK_NODATA;
        break;
    }
    case VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING: {
        const struct virtio_gpu_res_attach_backing *cmd =
            &payload->cmd.resource_attach_backing;
        struct vgpu_virgl_renderer_resource *res =
            vgpu_virgl_find_renderer_resource(cmd->resource_id);

        if (!res) {
            response_type = VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
            break;
        }
        if (res->backing_attached || payload->iov_count > INT_MAX) {
            response_type = VIRTIO_GPU_RESP_ERR_UNSPEC;
            break;
        }

        int ret = virgl_renderer_resource_attach_iov(
            cmd->resource_id, payload->iov, (int) payload->iov_count);
        if (ret) {
            response_type = VIRTIO_GPU_RESP_ERR_UNSPEC;
            break;
        }

        res->backing_attached = true;
        payload->iov = NULL;
        payload->iov_count = 0;
        response_type = VIRTIO_GPU_RESP_OK_NODATA;
        break;
    }
    case VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING: {
        const struct virtio_gpu_res_detach_backing *cmd =
            &payload->cmd.resource_detach_backing;
        struct vgpu_virgl_renderer_resource *res =
            vgpu_virgl_find_renderer_resource(cmd->resource_id);

        if (!res) {
            response_type = VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
            break;
        }
        if (!res->backing_attached) {
            response_type = VIRTIO_GPU_RESP_ERR_UNSPEC;
            break;
        }

        vgpu_virgl_detach_iov(cmd->resource_id);
        res->backing_attached = false;
        response_type = VIRTIO_GPU_RESP_OK_NODATA;
        break;
    }
    case VIRTIO_GPU_CMD_RESOURCE_MAP_BLOB: {
        const struct virtio_gpu_resource_map_blob *cmd =
            &payload->cmd.resource_map_blob;
        struct vgpu_virgl_renderer_resource *res;
        void *map_ptr = NULL;
        uint64_t map_size = 0;
        uint32_t map_info = 0;

        if (payload->response_capacity <
            sizeof(struct virtio_gpu_resp_map_info)) {
            response_type = VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
            break;
        }

        pthread_mutex_lock(&vgpu_virgl_resource_lock);
        res = vgpu_virgl_find_renderer_resource(cmd->resource_id);
        if (!res) {
            pthread_mutex_unlock(&vgpu_virgl_resource_lock);
            response_type = VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
            break;
        }
        if (!res->blob_resource) {
            pthread_mutex_unlock(&vgpu_virgl_resource_lock);
            response_type = VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
            break;
        }
        if (res->mapped) {
            pthread_mutex_unlock(&vgpu_virgl_resource_lock);
            response_type = VIRTIO_GPU_RESP_ERR_UNSPEC;
            break;
        }
        if (!vgpu_virgl_hostmem_range_valid(cmd->offset, res->blob_size)) {
            pthread_mutex_unlock(&vgpu_virgl_resource_lock);
            response_type = VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
            break;
        }
        if (virgl_renderer_resource_map(cmd->resource_id, &map_ptr,
                                        &map_size) != 0) {
            pthread_mutex_unlock(&vgpu_virgl_resource_lock);
            response_type = VIRTIO_GPU_RESP_ERR_UNSPEC;
            break;
        }
        if (virgl_renderer_resource_get_map_info(cmd->resource_id, &map_info) !=
            0) {
            (void) virgl_renderer_resource_unmap(cmd->resource_id);
            pthread_mutex_unlock(&vgpu_virgl_resource_lock);
            response_type = VIRTIO_GPU_RESP_ERR_UNSPEC;
            break;
        }
        if (!map_ptr || map_size < res->blob_size) {
            (void) virgl_renderer_resource_unmap(cmd->resource_id);
            pthread_mutex_unlock(&vgpu_virgl_resource_lock);
            response_type = VIRTIO_GPU_RESP_ERR_UNSPEC;
            break;
        }

        struct virtio_gpu_resp_map_info *out = calloc(1, sizeof(*out));
        if (!out) {
            (void) virgl_renderer_resource_unmap(cmd->resource_id);
            pthread_mutex_unlock(&vgpu_virgl_resource_lock);
            response_type = VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY;
            break;
        }
        out->hdr.type = VIRTIO_GPU_RESP_OK_MAP_INFO;
        out->hdr.flags = cmd->hdr.flags & (VIRTIO_GPU_FLAG_FENCE |
                                           VIRTIO_GPU_FLAG_INFO_RING_IDX);
        if (cmd->hdr.flags & VIRTIO_GPU_FLAG_FENCE)
            out->hdr.fence_id = cmd->hdr.fence_id;
        if (cmd->hdr.flags & VIRTIO_GPU_FLAG_INFO_RING_IDX)
            out->hdr.ring_idx = cmd->hdr.ring_idx;
        out->hdr.ctx_id = cmd->hdr.ctx_id;
        out->map_info = map_info;

        res->mapped = true;
        res->map_offset = cmd->offset;
        res->map_ptr = map_ptr;
        res->map_size = map_size;
        pthread_mutex_unlock(&vgpu_virgl_resource_lock);
        response = out;
        response_size = sizeof(*out);
        response_type = VIRTIO_GPU_RESP_OK_MAP_INFO;
        break;
    }
    case VIRTIO_GPU_CMD_RESOURCE_UNMAP_BLOB: {
        const struct virtio_gpu_resource_unmap_blob *cmd =
            &payload->cmd.resource_unmap_blob;
        struct vgpu_virgl_renderer_resource *res;

        pthread_mutex_lock(&vgpu_virgl_resource_lock);
        res = vgpu_virgl_find_renderer_resource(cmd->resource_id);
        if (!res) {
            pthread_mutex_unlock(&vgpu_virgl_resource_lock);
            response_type = VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
            break;
        }
        if (!res->blob_resource) {
            pthread_mutex_unlock(&vgpu_virgl_resource_lock);
            response_type = VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
            break;
        }
        if (res->mapped) {
            if (virgl_renderer_resource_unmap(cmd->resource_id) != 0) {
                pthread_mutex_unlock(&vgpu_virgl_resource_lock);
                response_type = VIRTIO_GPU_RESP_ERR_UNSPEC;
                break;
            }
            vgpu_virgl_clear_renderer_resource_map(res);
        }
        pthread_mutex_unlock(&vgpu_virgl_resource_lock);
        response_type = VIRTIO_GPU_RESP_OK_NODATA;
        break;
    }
    case VIRTIO_GPU_CMD_SET_SCANOUT:
        response_type = vgpu_virgl_record_renderer_scanout(payload);
        break;
    case VIRTIO_GPU_CMD_SET_SCANOUT_BLOB:
        response_type = vgpu_virgl_record_renderer_scanout_blob(payload);
        break;
    case VIRTIO_GPU_CMD_RESOURCE_UNREF: {
        const struct virtio_gpu_res_unref *cmd = &payload->cmd.resource_unref;
        struct vgpu_virgl_renderer_resource *res =
            vgpu_virgl_find_renderer_resource(cmd->resource_id);

        if (!res) {
            response_type = VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
            break;
        }

        vgpu_virgl_clear_renderer_resource_scanouts(cmd->resource_id);
        (void) vgpu_virgl_remove_renderer_resource(cmd->resource_id);
        virgl_renderer_resource_unref(cmd->resource_id);
        response_type = VIRTIO_GPU_RESP_OK_NODATA;
        break;
    }
    case VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D: {
        const struct virtio_gpu_transfer_host_3d *cmd =
            &payload->cmd.transfer_3d;
        struct vgpu_virgl_renderer_resource *res =
            vgpu_virgl_find_renderer_resource(cmd->resource_id);
        struct vgpu_virgl_box box = vgpu_virgl_box_from_virtio(&cmd->box);

        if (!res) {
            response_type = VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
            break;
        }
        if (!res->backing_attached) {
            response_type = VIRTIO_GPU_RESP_ERR_UNSPEC;
            break;
        }
        if (cmd->level > (uint32_t) INT_MAX) {
            response_type = VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
            break;
        }

        int ret = virgl_renderer_transfer_write_iov(
            cmd->resource_id, cmd->hdr.ctx_id, (int) cmd->level, cmd->stride,
            cmd->layer_stride, (struct virgl_box *) &box, cmd->offset, NULL, 0);
        response_type =
            ret ? VIRTIO_GPU_RESP_ERR_UNSPEC : VIRTIO_GPU_RESP_OK_NODATA;
        break;
    }
    case VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D: {
        const struct virtio_gpu_transfer_host_3d *cmd =
            &payload->cmd.transfer_3d;
        struct vgpu_virgl_renderer_resource *res =
            vgpu_virgl_find_renderer_resource(cmd->resource_id);
        struct vgpu_virgl_box box = vgpu_virgl_box_from_virtio(&cmd->box);

        if (!res) {
            response_type = VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
            break;
        }
        if (!res->backing_attached) {
            response_type = VIRTIO_GPU_RESP_ERR_UNSPEC;
            break;
        }
        if (cmd->level > (uint32_t) INT_MAX) {
            response_type = VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
            break;
        }

        int ret = virgl_renderer_transfer_read_iov(
            cmd->resource_id, cmd->hdr.ctx_id, cmd->level, cmd->stride,
            cmd->layer_stride, (struct virgl_box *) &box, cmd->offset, NULL, 0);
        response_type =
            ret ? VIRTIO_GPU_RESP_ERR_UNSPEC : VIRTIO_GPU_RESP_OK_NODATA;
        break;
    }
    case VIRTIO_GPU_CMD_SUBMIT_3D: {
        const struct virtio_gpu_cmd_submit *cmd = &payload->cmd.submit_3d;

        if (!payload->submit_data || payload->submit_data_size != cmd->size ||
            cmd->size == 0 || cmd->size % sizeof(uint32_t) != 0 ||
            cmd->size / sizeof(uint32_t) > (uint32_t) INT_MAX ||
            cmd->num_in_fences != 0) {
            response_type = VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
            break;
        }
        if (cmd->hdr.flags & VIRTIO_GPU_FLAG_FENCE) {
            response_type = VIRTIO_GPU_RESP_ERR_UNSPEC;
            break;
        }

        int ret =
            virgl_renderer_submit_cmd(payload->submit_data, cmd->hdr.ctx_id,
                                      (int) (cmd->size / sizeof(uint32_t)));
        response_type =
            ret ? VIRTIO_GPU_RESP_ERR_UNSPEC : VIRTIO_GPU_RESP_OK_NODATA;
        break;
    }
    default:
        response_type = VIRTIO_GPU_RESP_ERR_UNSPEC;
        break;
    }

    vgpu_virgl_complete_ctrl_request(request, payload, response_type, response,
                                     response_size);
}

bool vgpu_virgl_submit_fence(uint64_t generation,
                             bool context_fence,
                             uint32_t ctx_id,
                             uint32_t ring_idx,
                             uint64_t guest_fence_id)
{
    uint64_t renderer_fence_id;
    bool recorded;
    int ret;

    pthread_mutex_lock(&vgpu_virgl_lock);
    renderer_fence_id = context_fence
                            ? vgpu_virgl_alloc_context_renderer_fence_locked()
                            : vgpu_virgl_alloc_ctx0_renderer_fence_locked();
    recorded = vgpu_virgl_record_pending_fence(
        generation, context_fence, ctx_id, ring_idx, renderer_fence_id,
        guest_fence_id);
    pthread_mutex_unlock(&vgpu_virgl_lock);
    if (!recorded)
        return false;

    if (context_fence) {
        ret = virgl_renderer_context_create_fence(
            ctx_id, VIRGL_RENDERER_FENCE_FLAG_MERGEABLE, ring_idx,
            renderer_fence_id);
    } else {
        ret = virgl_renderer_create_fence((int) renderer_fence_id, 0);
    }

    if (ret) {
        vgpu_virgl_cancel_pending_fence(context_fence, ctx_id, ring_idx,
                                        renderer_fence_id);
        return false;
    }

    pthread_mutex_lock(&vgpu_virgl_lock);
    vgpu_virgl_request_poll_locked();
    pthread_mutex_unlock(&vgpu_virgl_lock);
    return true;
}

void vgpu_virgl_execute_renderer_request(
    const struct vgpu_renderer_request *request)
{
    if (!request)
        return;

    switch (request->type) {
    case VGPU_RENDERER_REQ_POLL:
        virgl_renderer_poll();
        pthread_mutex_lock(&vgpu_virgl_lock);
        debug_poll_requests_executed++;
        vgpu_virgl_poll_request_pending = false;
        pthread_mutex_unlock(&vgpu_virgl_lock);
        break;
    case VGPU_RENDERER_REQ_RESET:
        vgpu_virgl_reset_renderer();
        break;
    case VGPU_RENDERER_REQ_CTRL:
        vgpu_virgl_execute_ctrl_request(
            request, (struct vgpu_renderer_ctrl_payload *) request->payload);
        if (request->release_payload)
            request->release_payload(request->payload);
        break;
    default:
        break;
    }
}

void vgpu_virgl_debug_snapshot(struct vgpu_virgl_debug_stats *stats)
{
    if (!stats)
        return;

    pthread_mutex_lock(&vgpu_virgl_lock);
    *stats = (struct vgpu_virgl_debug_stats) {
        .pending_fences = vgpu_virgl_pending_fence_count,
        .poll_request_pending = vgpu_virgl_poll_request_pending,
        .poll_requests_submitted = debug_poll_requests_submitted,
        .poll_requests_dropped = debug_poll_requests_dropped,
        .poll_requests_executed = debug_poll_requests_executed,
        .fences_created = debug_fences_created,
        .fences_completed = debug_fences_completed,
        .last_ctx0_fence = debug_last_ctx0_fence,
        .last_context_ctx_id = debug_last_context_ctx_id,
        .last_context_ring_idx = debug_last_context_ring_idx,
        .last_context_fence = debug_last_context_fence,
    };
    pthread_mutex_unlock(&vgpu_virgl_lock);
}
