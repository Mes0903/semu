#pragma once

#if !SEMU_HAS(VIRTIOGPU)
#error Only valid when Virtio-GPU is enabled.
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/uio.h>

#include "vgpu-display.h"
#include "virtio-gpu.h"

#define VGPU_RENDERER_QUEUE_CAPACITY VIRTIO_GPU_QUEUE_NUM_MAX

enum vgpu_renderer_request_type {
    VGPU_RENDERER_REQ_INIT = 0,
    VGPU_RENDERER_REQ_RESET,
    VGPU_RENDERER_REQ_POLL,
    VGPU_RENDERER_REQ_CTRL,
    VGPU_RENDERER_REQ_SHUTDOWN,
};

enum vgpu_renderer_completion_type {
    VGPU_RENDERER_DONE_CTRL = 0,
    VGPU_RENDERER_DONE_VIRGL_RESOURCE,
    VGPU_RENDERER_DONE_FENCE,
    VGPU_RENDERER_DONE_FATAL,
};

enum vgpu_virgl_resource_side_effect_type {
    VGPU_VIRGL_RESOURCE_SIDE_EFFECT_NONE = 0,
    VGPU_VIRGL_RESOURCE_SIDE_EFFECT_CREATE_3D_ROLLBACK,
    VGPU_VIRGL_RESOURCE_SIDE_EFFECT_UNREF,
    VGPU_VIRGL_RESOURCE_SIDE_EFFECT_UNREF_ROLLBACK,
    VGPU_VIRGL_RESOURCE_SIDE_EFFECT_ATTACH_BACKING,
    VGPU_VIRGL_RESOURCE_SIDE_EFFECT_DETACH_BACKING,
    VGPU_VIRGL_RESOURCE_SIDE_EFFECT_SET_SCANOUT,
    VGPU_VIRGL_RESOURCE_SIDE_EFFECT_SET_SCANOUT_ROLLBACK,
};

struct vgpu_virgl_scanout_side_effect {
    uint32_t scanout_id;
    uint64_t scanout_generation;
    uint64_t resource_generation;
    bool has_gl_payload;
    struct vgpu_display_gl_payload gl_payload;
    struct vgpu_display_payload *display_payload;
    struct virtio_gpu_scanout_info scanout;
};

struct vgpu_renderer_token {
    uint32_t id;
    uint64_t generation;
};

struct vgpu_renderer_ctrl_payload {
    struct virtio_gpu_ctrl_hdr hdr;
    union {
        struct virtio_gpu_get_capset_info get_capset_info;
        struct virtio_gpu_get_capset get_capset;
        struct virtio_gpu_ctx_create ctx_create;
        struct virtio_gpu_ctx_destroy ctx_destroy;
        struct virtio_gpu_ctx_resource ctx_resource;
        struct virtio_gpu_res_unref resource_unref;
        struct virtio_gpu_res_attach_backing resource_attach_backing;
        struct virtio_gpu_res_detach_backing resource_detach_backing;
        struct virtio_gpu_set_scanout set_scanout;
        struct virtio_gpu_res_flush resource_flush;
        struct virtio_gpu_trans_to_host_2d transfer_to_host_2d;
        struct virtio_gpu_set_scanout_blob set_scanout_blob;
        struct virtio_gpu_resource_create_blob resource_create_blob;
        struct virtio_gpu_resource_map_blob resource_map_blob;
        struct virtio_gpu_resource_unmap_blob resource_unmap_blob;
        struct virtio_gpu_resource_create_3d resource_create_3d;
        struct virtio_gpu_transfer_host_3d transfer_3d;
        struct virtio_gpu_cmd_submit submit_3d;
    } cmd;
    struct iovec *iov;
    uint32_t iov_count;
    void *submit_data;
    size_t submit_data_size;
    uint64_t resource_generation;
    uint32_t scanout_id;
    uint64_t scanout_generation;
    bool has_gl_scanout_payload;
    struct vgpu_display_gl_payload gl_scanout_payload;
    struct virtio_gpu_scanout_info scanout;
    size_t response_capacity;
    uint32_t response_type;
    struct virtio_gpu_deferred_ctrl_completion ctrl_completion;
    struct virtq_desc response_desc;
};

struct vgpu_renderer_request {
    enum vgpu_renderer_request_type type;
    struct vgpu_renderer_token token;
    uint32_t command_type;
    void *payload;
    size_t payload_size;
    void (*release_payload)(void *payload);
};

struct vgpu_renderer_completion {
    enum vgpu_renderer_completion_type type;
    struct vgpu_renderer_token token;
    uint32_t response_type;
    void *response;
    size_t response_size;
    void (*release_response)(void *response);
    bool has_ctrl_completion;
    struct virtio_gpu_deferred_ctrl_completion ctrl_completion;
    bool has_response_desc;
    struct virtio_gpu_ctrl_hdr request_hdr;
    struct virtq_desc response_desc;
    struct {
        enum vgpu_virgl_resource_side_effect_type type;
        uint32_t resource_id;
        uint64_t resource_generation;
        bool backing_transition_success;
        uint32_t scanout_id;
        uint64_t scanout_generation;
        struct virtio_gpu_rect rect;
        struct vgpu_virgl_scanout_side_effect scanouts[VIRTIO_GPU_MAX_SCANOUTS];
        uint32_t scanout_count;
    } virgl_resource;
    bool context_fence;
    uint32_t ctx_id;
    uint32_t ring_idx;
    uint64_t fence_id;
};

struct vgpu_renderer_debug_stats {
    uint64_t active_generation;
    uint32_t request_head;
    uint32_t request_tail;
    uint32_t request_depth;
    uint32_t completion_head;
    uint32_t completion_tail;
    uint32_t completion_depth;
    bool available;
    bool resetting;
    uint64_t requests_submitted;
    uint64_t requests_dropped;
    uint64_t requests_popped;
    uint64_t completions_submitted;
    uint64_t completions_dropped;
    uint64_t completions_popped;
    uint64_t queue_resets;
    uint64_t execute_started;
    uint64_t execute_finished;
    uint64_t current_execute_seq;
    enum vgpu_renderer_request_type current_request_type;
    uint32_t current_command_type;
    uint32_t current_token_id;
    uint64_t current_generation;
};

void vgpu_renderer_set_wake_renderer(void (*wake_renderer)(void));
void vgpu_renderer_set_wake_frontend(void (*wake_frontend)(void));
/* Request payload ownership transfers only after a successful submit. A failed
 * request submit leaves payload ownership with the caller.
 */
bool vgpu_renderer_submit(const struct vgpu_renderer_request *request);
bool vgpu_renderer_pop_request(struct vgpu_renderer_request *request);
/* Completion response ownership transfers to the renderer completion path. If a
 * completion is rejected because it is stale, reset-gated, or the queue is
 * full, 'release_response' is invoked before returning false.
 */
bool vgpu_renderer_complete(const struct vgpu_renderer_completion *completion);
bool vgpu_renderer_pop_completion(struct vgpu_renderer_completion *completion);
/* Init/reset make the queues available for a generation. Shutdown drains
 * queued ownership hooks and leaves the queues unavailable until reopened.
 */
void vgpu_renderer_init_queues(uint64_t generation);
void vgpu_renderer_reset_queues(uint64_t generation);
void vgpu_renderer_shutdown_queues(void);
void vgpu_renderer_debug_note_execute_begin(
    const struct vgpu_renderer_request *request);
void vgpu_renderer_debug_note_execute_end(void);
void vgpu_renderer_debug_snapshot(struct vgpu_renderer_debug_stats *stats);
