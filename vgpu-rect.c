#include "vgpu-rect.h"

static uint32_t vgpu_rect_min_u32(uint32_t a, uint32_t b)
{
    return a < b ? a : b;
}

static uint32_t vgpu_rect_max_u32(uint32_t a, uint32_t b)
{
    return a > b ? a : b;
}

static uint64_t vgpu_dirty_rect_end_x(const struct vgpu_dirty_rect *rect)
{
    return (uint64_t) rect->x + rect->width;
}

static uint64_t vgpu_dirty_rect_end_y(const struct vgpu_dirty_rect *rect)
{
    return (uint64_t) rect->y + rect->height;
}

static bool vgpu_dirty_rect_valid(const struct vgpu_dirty_rect *rect)
{
    return rect && rect->width != 0 && rect->height != 0 &&
           vgpu_dirty_rect_end_x(rect) <= UINT32_MAX &&
           vgpu_dirty_rect_end_y(rect) <= UINT32_MAX;
}

static uint64_t vgpu_dirty_rect_area(const struct vgpu_dirty_rect *rect)
{
    return (uint64_t) rect->width * rect->height;
}

bool vgpu_dirty_rect_fits(uint32_t width,
                          uint32_t height,
                          const struct vgpu_dirty_rect *rect)
{
    if (!vgpu_dirty_rect_valid(rect))
        return false;
    if (rect->x >= width || rect->y >= height)
        return false;

    return rect->width <= width - rect->x && rect->height <= height - rect->y;
}

bool vgpu_dirty_rect_intersect(const struct vgpu_dirty_rect *a,
                               const struct vgpu_dirty_rect *b,
                               struct vgpu_dirty_rect *out)
{
    if (!vgpu_dirty_rect_valid(a) || !vgpu_dirty_rect_valid(b))
        return false;

    uint32_t x1 = vgpu_rect_max_u32(a->x, b->x);
    uint32_t y1 = vgpu_rect_max_u32(a->y, b->y);
    uint64_t x2 = vgpu_dirty_rect_end_x(a) < vgpu_dirty_rect_end_x(b)
                      ? vgpu_dirty_rect_end_x(a)
                      : vgpu_dirty_rect_end_x(b);
    uint64_t y2 = vgpu_dirty_rect_end_y(a) < vgpu_dirty_rect_end_y(b)
                      ? vgpu_dirty_rect_end_y(a)
                      : vgpu_dirty_rect_end_y(b);

    if ((uint64_t) x1 >= x2 || (uint64_t) y1 >= y2)
        return false;

    if (out) {
        out->x = x1;
        out->y = y1;
        out->width = (uint32_t) (x2 - x1);
        out->height = (uint32_t) (y2 - y1);
    }
    return true;
}

bool vgpu_dirty_rect_merge_exact(const struct vgpu_dirty_rect *a,
                                 const struct vgpu_dirty_rect *b,
                                 struct vgpu_dirty_rect *out)
{
    struct vgpu_dirty_rect intersection;
    uint64_t intersection_area = 0;

    if (!vgpu_dirty_rect_valid(a) || !vgpu_dirty_rect_valid(b) || !out)
        return false;

    uint32_t x1 = vgpu_rect_min_u32(a->x, b->x);
    uint32_t y1 = vgpu_rect_min_u32(a->y, b->y);
    uint64_t x2 = vgpu_dirty_rect_end_x(a) > vgpu_dirty_rect_end_x(b)
                      ? vgpu_dirty_rect_end_x(a)
                      : vgpu_dirty_rect_end_x(b);
    uint64_t y2 = vgpu_dirty_rect_end_y(a) > vgpu_dirty_rect_end_y(b)
                      ? vgpu_dirty_rect_end_y(a)
                      : vgpu_dirty_rect_end_y(b);
    struct vgpu_dirty_rect bounds = {
        .x = x1,
        .y = y1,
        .width = (uint32_t) (x2 - x1),
        .height = (uint32_t) (y2 - y1),
    };

    if (!vgpu_dirty_rect_valid(&bounds))
        return false;

    if (vgpu_dirty_rect_intersect(a, b, &intersection))
        intersection_area = vgpu_dirty_rect_area(&intersection);

    uint64_t union_area =
        vgpu_dirty_rect_area(a) + vgpu_dirty_rect_area(b) - intersection_area;
    if (vgpu_dirty_rect_area(&bounds) != union_area)
        return false;

    *out = bounds;
    return true;
}

bool vgpu_rect_compute_update(const struct vgpu_dirty_rect *scanout_src,
                              const struct vgpu_dirty_rect *flush,
                              struct vgpu_rect_update *out)
{
    struct vgpu_dirty_rect src;

    if (!out || !vgpu_dirty_rect_intersect(scanout_src, flush, &src))
        return false;

    out->src = src;
    out->dst = (struct vgpu_dirty_rect) {
        .x = src.x - scanout_src->x,
        .y = src.y - scanout_src->y,
        .width = src.width,
        .height = src.height,
    };
    return true;
}

bool vgpu_rect_compute_full_update(const struct vgpu_dirty_rect *scanout_src,
                                   struct vgpu_rect_update *out)
{
    if (!out || !vgpu_dirty_rect_valid(scanout_src))
        return false;

    out->src = *scanout_src;
    out->dst = (struct vgpu_dirty_rect) {
        .x = 0,
        .y = 0,
        .width = scanout_src->width,
        .height = scanout_src->height,
    };
    return true;
}
