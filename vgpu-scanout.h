#pragma once

#if !SEMU_HAS(VIRTIOGPU)
#error Only valid when Virtio-GPU is enabled.
#endif

#include <stdbool.h>
#include <stdint.h>

#include "vgpu-display.h"
#include "virtio-gpu.h"

static inline void vgpu_scanout_primary_reset_dirty_state(
    struct virtio_gpu_scanout_info *scanout,
    uint32_t generation)
{
    scanout->primary_dirty = (struct vgpu_scanout_dirty_state) {
        .primary_clear_generation = generation,
    };
}

static inline bool vgpu_scanout_primary_bind_if_generation_advanced(
    struct virtio_gpu_scanout_info *scanout,
    bool generation_advanced,
    uint32_t resource_id,
    const struct vgpu_dirty_rect *src,
    uint32_t generation)
{
    if (!scanout || !generation_advanced || !src)
        return false;

    scanout->primary_resource_id = resource_id;
    scanout->src_x = src->x;
    scanout->src_y = src->y;
    scanout->src_w = src->width;
    scanout->src_h = src->height;
    vgpu_scanout_primary_reset_dirty_state(scanout, generation);
    return true;
}

static inline bool vgpu_scanout_primary_clear_if_published(
    struct virtio_gpu_scanout_info *scanout,
    enum vgpu_display_publish_result result,
    uint32_t generation)
{
    if (!scanout || !vgpu_display_lifecycle_publish_succeeded(result))
        return false;

    scanout->primary_resource_id = 0;
    scanout->src_x = 0;
    scanout->src_y = 0;
    scanout->src_w = 0;
    scanout->src_h = 0;
    vgpu_scanout_primary_reset_dirty_state(scanout, generation);
    return true;
}
