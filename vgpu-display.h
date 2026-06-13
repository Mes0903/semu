#pragma once

#if !SEMU_HAS(VIRTIOGPU)
#error Only valid when Virtio-GPU is enabled.
#endif

#include <stdbool.h>
#include <stdint.h>

#include "virtio-gpu.h"

/* Immutable CPU-frame payload published by the VirtIO GPU backend and later
 * consumed by the window backend when it uploads pixels into its own textures.
 * 'width'/'height' describe the packed payload pixels. 'texture_*' is
 * the retained texture size for the plane, and 'dst_*' describes the
 * destination rectangle inside that texture.
 */
struct vgpu_display_cpu_payload {
    enum virtio_gpu_formats format;
    uint32_t width, height;
    uint32_t stride;
    uint32_t bits_per_pixel;
    uint32_t texture_width, texture_height;
    uint32_t dst_x, dst_y, dst_width, dst_height;
    uint8_t *pixels;
};

/* Owning payload object passed through the display queue. The bridge queues
 * and disposes this object, while GPU and window backends only fill or
 * consume the payload it carries.
 */
struct vgpu_display_payload {
    struct vgpu_display_cpu_payload cpu;
    /* TODO: Add a GL/virgl payload when 3D scanout is implemented. The display
     * bridge currently transports CPU-owned 2D frames only.
     */
};

enum vgpu_display_publish_result {
    VGPU_DISPLAY_PUBLISH_OK = 0,
    VGPU_DISPLAY_PUBLISH_UNAVAILABLE,
    VGPU_DISPLAY_PUBLISH_QUEUE_FULL,
    VGPU_DISPLAY_PUBLISH_BACKPRESSURE,
};

static inline bool vgpu_display_lifecycle_publish_succeeded(
    enum vgpu_display_publish_result result)
{
    return result == VGPU_DISPLAY_PUBLISH_OK ||
           result == VGPU_DISPLAY_PUBLISH_UNAVAILABLE;
}

/* Runtime display commands published by the GPU backend and consumed by the
 * window backend. 'PRIMARY_*' updates the main scanout image, while 'CURSOR_*'
 * updates or moves the separate cursor plane.
 *
 * Clear commands are reliable generation changes, frame/move commands are lossy
 * SPSC queue entries. A failed frame publish leaves payload ownership with the
 * caller so producer-side dirty state can be retained and retried.
 */
enum vgpu_display_cmd_type {
    VGPU_DISPLAY_CMD_PRIMARY_SET = 0,
    VGPU_DISPLAY_CMD_PRIMARY_CLEAR,
    VGPU_DISPLAY_CMD_CURSOR_SET,
    VGPU_DISPLAY_CMD_CURSOR_CLEAR,
    VGPU_DISPLAY_CMD_CURSOR_MOVE,
};

/* One synthesized display bridge command. 'scanout_id' selects which scanout
 * to update, and the union carries the payload or coordinates required by the
 * specific command type above.
 */
struct vgpu_display_cmd {
    enum vgpu_display_cmd_type type;
    uint32_t scanout_id;
    uint32_t generation;
    union {
        struct {
            struct vgpu_display_payload *payload;
        } primary_set;
        struct {
            struct vgpu_display_payload *payload;
            int32_t x;
            int32_t y;
            uint32_t hot_x;
            uint32_t hot_y;
        } cursor_set;
        struct {
            int32_t x;
            int32_t y;
        } cursor_move;
    } u;
};

void vgpu_display_set_scanout_count(uint32_t scanout_count);
uint32_t vgpu_display_primary_generation(uint32_t scanout_id);
uint32_t vgpu_display_advance_primary_generation(uint32_t scanout_id);
enum vgpu_display_publish_result vgpu_display_publish_primary_clear(
    uint32_t scanout_id);
enum vgpu_display_publish_result vgpu_display_publish_cursor_clear(
    uint32_t scanout_id);

void vgpu_display_release_cmd(struct vgpu_display_cmd *cmd);
bool vgpu_display_pop_cmd(struct vgpu_display_cmd *cmd);
void vgpu_display_set_unavailable(void);
/* Shutdown-only handoff after the GPU producer/actor has stopped. Marks the
 * display bridge unavailable, then drains and releases queued payloads before
 * the window backend tears down its consumer-owned state.
 */
void vgpu_display_shutdown_after_producer_stopped(void);
bool vgpu_display_can_publish(void);
bool vgpu_display_cpu_payload_is_full_texture_update(
    const struct vgpu_display_cpu_payload *payload);
enum vgpu_display_publish_result vgpu_display_publish_primary_set(
    uint32_t scanout_id,
    struct vgpu_display_payload *payload);
enum vgpu_display_publish_result vgpu_display_publish_cursor_set(
    uint32_t scanout_id,
    struct vgpu_display_payload *payload,
    int32_t x,
    int32_t y,
    uint32_t hot_x,
    uint32_t hot_y);
enum vgpu_display_publish_result
vgpu_display_publish_cursor_move(uint32_t scanout_id, int32_t x, int32_t y);
