#pragma once

#if !SEMU_HAS(VIRGL)
#error Only valid when VirGL is enabled.
#endif

#include <stdbool.h>
#include <stdint.h>

#include <virglrenderer.h>

#include "vgpu-renderer.h"

struct vgpu_virgl_debug_stats {
    uint32_t pending_fences;
    bool poll_request_pending;
    uint64_t poll_requests_submitted;
    uint64_t poll_requests_dropped;
    uint64_t poll_requests_executed;
    uint64_t fences_created;
    uint64_t fences_completed;
    uint64_t last_ctx0_fence;
    uint32_t last_context_ctx_id;
    uint32_t last_context_ring_idx;
    uint64_t last_context_fence;
};

int vgpu_virgl_init_renderer(void *cookie);
void vgpu_virgl_reset_renderer(void);
bool vgpu_virgl_submit_fence(uint64_t generation,
                             bool context_fence,
                             uint32_t ctx_id,
                             uint32_t ring_idx,
                             uint64_t guest_fence_id);
void vgpu_virgl_execute_renderer_request(
    const struct vgpu_renderer_request *request);
void vgpu_virgl_debug_snapshot(struct vgpu_virgl_debug_stats *stats);
bool vgpu_virgl_hostmem_read(uint64_t off, uint8_t width, uint32_t *value);
bool vgpu_virgl_hostmem_write(uint64_t off, uint8_t width, uint32_t value);

virgl_renderer_gl_context vgpu_window_virgl_create_context(
    int scanout_idx,
    struct virgl_renderer_gl_ctx_param *param);
void vgpu_window_virgl_destroy_context(virgl_renderer_gl_context ctx);
int vgpu_window_virgl_make_current(int scanout_idx,
                                   virgl_renderer_gl_context ctx);
