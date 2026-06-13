#pragma once

#include <stdbool.h>
#include <stdint.h>

struct vgpu_dirty_rect {
    uint32_t x, y, width, height;
};

struct vgpu_scanout_dirty_state {
    bool dirty;
    bool needs_full_resync;
    struct vgpu_dirty_rect rect;
    uint32_t primary_clear_generation;
};

struct vgpu_rect_update {
    struct vgpu_dirty_rect src;
    struct vgpu_dirty_rect dst;
};

bool vgpu_dirty_rect_fits(uint32_t width,
                          uint32_t height,
                          const struct vgpu_dirty_rect *rect);
bool vgpu_dirty_rect_intersect(const struct vgpu_dirty_rect *a,
                               const struct vgpu_dirty_rect *b,
                               struct vgpu_dirty_rect *out);
bool vgpu_dirty_rect_merge_exact(const struct vgpu_dirty_rect *a,
                                 const struct vgpu_dirty_rect *b,
                                 struct vgpu_dirty_rect *out);
bool vgpu_rect_compute_update(const struct vgpu_dirty_rect *scanout_src,
                              const struct vgpu_dirty_rect *flush,
                              struct vgpu_rect_update *out);
bool vgpu_rect_compute_full_update(const struct vgpu_dirty_rect *scanout_src,
                                   struct vgpu_rect_update *out);
