#include "virtio-gpu-virgl.h"

#include <pthread.h>
#include <stddef.h>
#include <string.h>

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

    virgl_renderer_reset();
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
