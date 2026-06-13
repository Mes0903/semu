#include <SDL.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#if SEMU_HAS(VIRGL)
#include <epoxy/gl.h>

#include "vgpu-renderer.h"
#include "virtio-gpu-virgl.h"
#endif
#if SEMU_HAS(VIRTIOGPU)
#include "vgpu-display.h"
#include "virtio-gpu.h"
#endif
#if SEMU_HAS(VIRTIOINPUT)
#include "virtio-input-event.h"
#endif
#include "window.h"

#define WINDOW_LOG_PREFIX "[SEMU WINDOW] "

static int wake_write_fd = -1;
static bool sdl_initialized = false;
static bool headless_mode = false;
static bool should_exit = false;

#if SEMU_HAS(VIRTIOINPUT)
static bool mouse_grabbed = false;
static SDL_Window *sdl_input_window;
#else
#define SDL_EVENT_WAIT_TIMEOUT_MS 1 /* ms */
#define SDL_EVENT_BURST_LIMIT 64U
#endif

#if SEMU_HAS(VIRTIOGPU)
/* SDL-owned retained state for a single plane. Textures live only on the SDL
 * thread and are updated from immutable CPU-frame display resources.
 */
struct sdl_plane_info {
    uint32_t width;
    uint32_t height;
    uint32_t sdl_format;
    bool alpha_blend;
    SDL_Texture *texture;
};

/* SDL-owned retained state for one scanout. 'window_init_sw()' creates the
 * window/renderer, then 'window_drain_display_queue()' updates the primary and
 * cursor planes from queued display payloads before rendering them.
 */
struct sdl_scanout_info {
    struct sdl_plane_info primary_plane;
    struct sdl_plane_info cursor_plane;
    SDL_Rect cursor_rect;
    uint32_t cursor_hot_x;
    uint32_t cursor_hot_y;
    uint32_t window_width;
    uint32_t window_height;

    SDL_Window *window;
    SDL_Renderer *renderer;
#if SEMU_HAS(VIRGL)
    SDL_GLContext gl_context;
    GLuint gl_primary_fb;
    GLuint gl_primary_cpu_texture;
    GLuint gl_cursor_texture;
    uint32_t gl_primary_cpu_width;
    uint32_t gl_primary_cpu_height;
    uint32_t gl_cursor_width;
    uint32_t gl_cursor_height;
    bool gl_primary_valid;
    bool gl_primary_cpu_valid;
    bool gl_cursor_valid;
    struct vgpu_display_gl_payload gl_primary;
#endif
};

static struct sdl_scanout_info sdl_scanouts[VIRTIO_GPU_MAX_SCANOUTS];
#endif

static void window_set_wake_fd_sw(int fd)
{
    wake_write_fd = fd;
}

static void window_wake_backend_sw(void)
{
    if (wake_write_fd >= 0) {
        char byte = 1;
        /* Best-effort wakeup: the pipe is non-blocking, and the byte value has
         * no meaning beyond making the read end readable.
         */
        ssize_t bytes_written = write(wake_write_fd, &byte, 1);
        (void) bytes_written;
    }
}

#if SEMU_HAS(VIRGL)
static void window_wake_renderer_sw(void)
{
    if (!sdl_initialized || headless_mode)
        return;

    SDL_Event event = {
        .type = SDL_USEREVENT,
    };
    int ret = SDL_PushEvent(&event);
    (void) ret;
}
#endif

static void window_shutdown_sw(void)
{
    /* Both user-driven close and emulator-driven shutdown funnel through the
     * same flag so the main thread and emulator thread observe one exit state.
     */
    __atomic_store_n(&should_exit, true, __ATOMIC_RELAXED);
    /* Unblock any 'poll(-1)' in the SMP emulator loop immediately. */
    window_wake_backend_sw();
}

static bool window_is_closed_sw(void)
{
    return __atomic_load_n(&should_exit, __ATOMIC_RELAXED);
}

#if SEMU_HAS(VIRTIOINPUT)
/* Main-thread-only helper for relative-pointer devices. SDL's grab and
 * relative mouse APIs are part of the windowing backend, so callers use this
 * to switch between normal host-pointer mode and guest-directed mouse mode.
 */
static void window_set_mouse_grab_sw(bool grabbed)
{
    if (headless_mode || !sdl_input_window) {
        mouse_grabbed = false;
        return;
    }

    if (mouse_grabbed == grabbed)
        return;

    if (grabbed) {
        if (SDL_SetRelativeMouseMode(SDL_TRUE) < 0) {
            fprintf(stderr,
                    "window_set_mouse_grab_sw(): failed to enable relative "
                    "mouse mode: %s\n",
                    SDL_GetError());
            return;
        }
        SDL_SetWindowGrab(sdl_input_window, SDL_TRUE);
        SDL_ShowCursor(SDL_DISABLE);
    } else {
        SDL_SetWindowGrab(sdl_input_window, SDL_FALSE);
        SDL_SetRelativeMouseMode(SDL_FALSE);
        SDL_ShowCursor(SDL_ENABLE);
    }

    mouse_grabbed = grabbed;
}

static bool window_is_mouse_grabbed_sw(void)
{
    return mouse_grabbed;
}
#endif

#if SEMU_HAS(VIRTIOGPU)
static bool vgpu_format_to_sdl_format(enum virtio_gpu_formats virtio_gpu_format,
                                      uint32_t *sdl_format)
{
    switch (virtio_gpu_format) {
    case VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM:
        *sdl_format = SDL_PIXELFORMAT_ARGB8888;
        return true;
    case VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM:
        *sdl_format = SDL_PIXELFORMAT_XRGB8888;
        return true;
    case VIRTIO_GPU_FORMAT_A8R8G8B8_UNORM:
        *sdl_format = SDL_PIXELFORMAT_BGRA8888;
        return true;
    case VIRTIO_GPU_FORMAT_X8R8G8B8_UNORM:
        *sdl_format = SDL_PIXELFORMAT_BGRX8888;
        return true;
    case VIRTIO_GPU_FORMAT_R8G8B8A8_UNORM:
        *sdl_format = SDL_PIXELFORMAT_ABGR8888;
        return true;
    case VIRTIO_GPU_FORMAT_X8B8G8R8_UNORM:
        *sdl_format = SDL_PIXELFORMAT_RGBX8888;
        return true;
    case VIRTIO_GPU_FORMAT_A8B8G8R8_UNORM:
        *sdl_format = SDL_PIXELFORMAT_RGBA8888;
        return true;
    case VIRTIO_GPU_FORMAT_R8G8B8X8_UNORM:
        *sdl_format = SDL_PIXELFORMAT_XBGR8888;
        return true;
    default:
        return false;
    }
}

static void sdl_plane_info_reset(struct sdl_plane_info *plane)
{
    bool alpha_blend = plane->alpha_blend;
    if (plane->texture)
        SDL_DestroyTexture(plane->texture);
    memset(plane, 0, sizeof(*plane));
    plane->alpha_blend = alpha_blend;
}

static void sdl_plane_info_cleanup(struct sdl_plane_info *plane)
{
    if (plane->texture)
        SDL_DestroyTexture(plane->texture);
    memset(plane, 0, sizeof(*plane));
}

#if SEMU_HAS(VIRGL)
static void sdl_scanout_detach_gl_context(void)
{
    int ret = SDL_GL_MakeCurrent(NULL, NULL);
    (void) ret;
}
#endif

static void sdl_scanout_info_cleanup(struct sdl_scanout_info *scanout)
{
    sdl_plane_info_cleanup(&scanout->primary_plane);
    sdl_plane_info_cleanup(&scanout->cursor_plane);

#if SEMU_HAS(VIRGL)
    bool gl_current = false;
    if (scanout->gl_context && scanout->window)
        gl_current =
            SDL_GL_MakeCurrent(scanout->window, scanout->gl_context) == 0;
    if (gl_current && scanout->gl_primary_fb)
        glDeleteFramebuffers(1, &scanout->gl_primary_fb);
    if (gl_current && scanout->gl_primary_cpu_texture)
        glDeleteTextures(1, &scanout->gl_primary_cpu_texture);
    if (gl_current && scanout->gl_cursor_texture)
        glDeleteTextures(1, &scanout->gl_cursor_texture);
    if (gl_current)
        sdl_scanout_detach_gl_context();
    if (scanout->gl_context)
        SDL_GL_DeleteContext(scanout->gl_context);
#endif

    if (scanout->renderer)
        SDL_DestroyRenderer(scanout->renderer);
    if (scanout->window)
        SDL_DestroyWindow(scanout->window);

    memset(scanout, 0, sizeof(*scanout));
}

static bool sdl_scanout_is_ready(const struct sdl_scanout_info *scanout)
{
    if (!scanout || !scanout->window)
        return false;

#if SEMU_HAS(VIRGL)
    if (scanout->gl_context)
        return true;
#endif
    return scanout->renderer != NULL;
}

static void sdl_scanout_clear_primary(struct sdl_scanout_info *scanout)
{
    sdl_plane_info_reset(&scanout->primary_plane);
#if SEMU_HAS(VIRGL)
    scanout->gl_primary_cpu_width = 0;
    scanout->gl_primary_cpu_height = 0;
    scanout->gl_primary_valid = false;
    scanout->gl_primary_cpu_valid = false;
#endif
}

static void sdl_scanout_clear_cursor(struct sdl_scanout_info *scanout)
{
    memset(&scanout->cursor_rect, 0, sizeof(scanout->cursor_rect));
    scanout->cursor_hot_x = 0;
    scanout->cursor_hot_y = 0;
    sdl_plane_info_reset(&scanout->cursor_plane);
#if SEMU_HAS(VIRGL)
    scanout->gl_cursor_width = 0;
    scanout->gl_cursor_height = 0;
    scanout->gl_cursor_valid = false;
#endif
}

static bool sdl_plane_info_get_sdl_format(
    const struct sdl_plane_info *plane,
    const struct vgpu_display_payload *payload,
    uint32_t *sdl_format)
{
    /* The plane keeps its SDL objects across frames, but the payload format is
     * still per-update data. Resolve the incoming VirtIO-GPU format first,
     * then adjust it below if this plane requires alpha.
     */
    const struct vgpu_display_cpu_payload *frame = &payload->cpu;
    if (!vgpu_format_to_sdl_format(frame->format, sdl_format)) {
        fprintf(stderr, "%s(): invalid resource format %u\n", __func__,
                (uint32_t) frame->format);
        return false;
    }

    /* Cursor textures need an alpha-capable SDL format. If the incoming format
     * is an XRGB/XBGR/BGRX/RGBX variant, switch to the matching alpha version
     * so the high byte is preserved as transparency instead of being ignored.
     */
    if (plane->alpha_blend) {
        switch (*sdl_format) {
        case SDL_PIXELFORMAT_XRGB8888:
            *sdl_format = SDL_PIXELFORMAT_ARGB8888;
            break;
        case SDL_PIXELFORMAT_BGRX8888:
            *sdl_format = SDL_PIXELFORMAT_BGRA8888;
            break;
        case SDL_PIXELFORMAT_RGBX8888:
            *sdl_format = SDL_PIXELFORMAT_RGBA8888;
            break;
        case SDL_PIXELFORMAT_XBGR8888:
            *sdl_format = SDL_PIXELFORMAT_ABGR8888;
            break;
        default:
            break;
        }
    }

    return true;
}

static SDL_Texture *sdl_plane_info_create_texture(
    SDL_Renderer *renderer,
    const struct sdl_plane_info *plane,
    const struct vgpu_display_cpu_payload *frame,
    uint32_t sdl_format)
{
    SDL_Texture *texture =
        SDL_CreateTexture(renderer, sdl_format, SDL_TEXTUREACCESS_STREAMING,
                          frame->texture_width, frame->texture_height);
    if (!texture) {
        fprintf(stderr, "%s(): failed to create texture: %s\n", __func__,
                SDL_GetError());
        return NULL;
    }

    if (plane->alpha_blend) {
        if (SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND) < 0) {
            fprintf(stderr, "%s(): failed to enable texture blending: %s\n",
                    __func__, SDL_GetError());
        }
    }

    return texture;
}


static bool sdl_frame_rect_to_sdl(const struct vgpu_display_cpu_payload *frame,
                                  SDL_Rect *rect)
{
    if (frame->dst_x > (uint32_t) INT_MAX ||
        frame->dst_y > (uint32_t) INT_MAX ||
        frame->dst_width > (uint32_t) INT_MAX ||
        frame->dst_height > (uint32_t) INT_MAX)
        return false;

    rect->x = (int) frame->dst_x;
    rect->y = (int) frame->dst_y;
    rect->w = (int) frame->dst_width;
    rect->h = (int) frame->dst_height;
    return true;
}

static bool sdl_frame_rect_fits_plane(
    const struct vgpu_display_cpu_payload *frame,
    const struct sdl_plane_info *plane)
{
    return frame->dst_x < plane->width && frame->dst_y < plane->height &&
           frame->dst_width <= plane->width - frame->dst_x &&
           frame->dst_height <= plane->height - frame->dst_y;
}

static bool sdl_plane_info_update_texture(
    SDL_Renderer *renderer,
    struct sdl_plane_info *plane,
    const struct vgpu_display_payload *payload,
    const char *plane_name)
{
    if (!payload || payload->type != VGPU_DISPLAY_PAYLOAD_CPU) {
        fprintf(stderr, "%s(): unsupported %s display payload type %u\n",
                __func__, plane_name, payload ? (uint32_t) payload->type : 0);
        return false;
    }

    const struct vgpu_display_cpu_payload *frame = &payload->cpu;
    uint32_t sdl_format;
    if (!sdl_plane_info_get_sdl_format(plane, payload, &sdl_format))
        return false;

    if (frame->width == 0 || frame->height == 0 || frame->texture_width == 0 ||
        frame->texture_height == 0 || frame->dst_width == 0 ||
        frame->dst_height == 0 || frame->dst_width != frame->width ||
        frame->dst_height != frame->height ||
        frame->dst_x >= frame->texture_width ||
        frame->dst_y >= frame->texture_height ||
        frame->dst_width > frame->texture_width - frame->dst_x ||
        frame->dst_height > frame->texture_height - frame->dst_y) {
        fprintf(stderr,
                "%s(): invalid %s frame metadata payload=%ux%u dst=%u,%u "
                "%ux%u\n",
                __func__, plane_name, frame->width, frame->height, frame->dst_x,
                frame->dst_y, frame->dst_width, frame->dst_height);
        return false;
    }

    bool full_update = vgpu_display_cpu_payload_is_full_texture_update(frame);
    bool reuse_texture = plane->texture &&
                         plane->width == frame->texture_width &&
                         plane->height == frame->texture_height &&
                         plane->sdl_format == sdl_format;
    SDL_Texture *texture = plane->texture;
    SDL_Rect rect;
    SDL_Rect *update_rect = NULL;

    if (full_update) {
        if (!reuse_texture) {
            texture = sdl_plane_info_create_texture(renderer, plane, frame,
                                                    sdl_format);
            if (!texture)
                return false;
        }
    } else {
        if (!plane->texture) {
            fprintf(stderr,
                    "%s(): rejecting partial %s update without texture\n",
                    __func__, plane_name);
            return false;
        }
        if (plane->sdl_format != sdl_format ||
            plane->width != frame->texture_width ||
            plane->height != frame->texture_height) {
            fprintf(stderr,
                    "%s(): rejecting partial %s update with texture mismatch\n",
                    __func__, plane_name);
            return false;
        }
        if (!sdl_frame_rect_fits_plane(frame, plane) ||
            !sdl_frame_rect_to_sdl(frame, &rect)) {
            fprintf(stderr,
                    "%s(): rejecting partial %s update outside texture\n",
                    __func__, plane_name);
            return false;
        }
        update_rect = &rect;
    }

    /* Keep the retained plane state unchanged until the new pixels are known
     * to be uploaded successfully.
     */
    if (SDL_UpdateTexture(texture, update_rect, frame->pixels, frame->stride) !=
        0) {
        fprintf(stderr, "%s(): failed to update %s texture: %s\n", __func__,
                plane_name, SDL_GetError());
        if (full_update && !reuse_texture)
            SDL_DestroyTexture(texture);
        return false;
    }

    if (full_update && !reuse_texture) {
        if (plane->texture)
            SDL_DestroyTexture(plane->texture);
        plane->texture = texture;
        plane->width = frame->texture_width;
        plane->height = frame->texture_height;
        plane->sdl_format = sdl_format;
    }
    return true;
}

static bool sdl_cursor_rect_update_position(SDL_Rect *rect,
                                            int32_t x,
                                            int32_t y,
                                            uint32_t hot_x,
                                            uint32_t hot_y)
{
    int64_t rect_x = (int64_t) x - (int64_t) hot_x;
    int64_t rect_y = (int64_t) y - (int64_t) hot_y;

    if (rect_x < INT_MIN || rect_x > INT_MAX || rect_y < INT_MIN ||
        rect_y > INT_MAX) {
        fprintf(stderr,
                WINDOW_LOG_PREFIX
                "%s(): cursor position out of SDL range "
                "(x=%" PRId32 " y=%" PRId32 " hot_x=%u hot_y=%u)\n",
                __func__, x, y, (unsigned) hot_x, (unsigned) hot_y);
        return false;
    }

    rect->x = (int) rect_x;
    rect->y = (int) rect_y;
    return true;
}

static bool sdl_scanout_apply_cursor_frame(
    struct sdl_scanout_info *scanout,
    const struct vgpu_display_payload *payload,
    int32_t x,
    int32_t y,
    uint32_t hot_x,
    uint32_t hot_y)
{
    if (!payload || payload->type != VGPU_DISPLAY_PAYLOAD_CPU) {
        fprintf(stderr,
                WINDOW_LOG_PREFIX
                "%s(): unsupported cursor display payload type %u\n",
                __func__, payload ? (uint32_t) payload->type : 0);
        return false;
    }

    const struct vgpu_display_cpu_payload *frame = &payload->cpu;
    struct sdl_plane_info *plane = &scanout->cursor_plane;
    SDL_Rect new_cursor_rect = scanout->cursor_rect;

    if (frame->width > INT_MAX || frame->height > INT_MAX) {
        fprintf(stderr,
                WINDOW_LOG_PREFIX
                "%s(): cursor size out of SDL range (%ux%u)\n",
                __func__, frame->width, frame->height);
        return false;
    }

    if (!sdl_cursor_rect_update_position(&new_cursor_rect, x, y, hot_x, hot_y))
        return false;

    if (!sdl_plane_info_update_texture(scanout->renderer, plane, payload,
                                       "cursor"))
        return false;

    scanout->cursor_hot_x = hot_x;
    scanout->cursor_hot_y = hot_y;
    new_cursor_rect.w = (int) frame->width;
    new_cursor_rect.h = (int) frame->height;
    scanout->cursor_rect = new_cursor_rect;
    return true;
}

#if SEMU_HAS(VIRGL)
static bool sdl_scanout_apply_gl_cpu_primary_frame(
    struct sdl_scanout_info *scanout,
    const struct vgpu_display_payload *payload)
{
    if (!scanout->gl_context || !payload ||
        payload->type != VGPU_DISPLAY_PAYLOAD_CPU)
        return false;

    const struct vgpu_display_cpu_payload *frame = &payload->cpu;
    uint32_t src_sdl_format;
    if (!sdl_plane_info_get_sdl_format(&scanout->primary_plane, payload,
                                       &src_sdl_format))
        return false;

    if (frame->width == 0 || frame->height == 0 || frame->texture_width == 0 ||
        frame->texture_height == 0 || frame->dst_width == 0 ||
        frame->dst_height == 0 || frame->dst_width != frame->width ||
        frame->dst_height != frame->height ||
        frame->dst_x >= frame->texture_width ||
        frame->dst_y >= frame->texture_height ||
        frame->dst_width > frame->texture_width - frame->dst_x ||
        frame->dst_height > frame->texture_height - frame->dst_y ||
        frame->texture_width > INT_MAX || frame->texture_height > INT_MAX) {
        fprintf(stderr,
                "%s(): invalid GL primary CPU frame metadata payload=%ux%u "
                "dst=%u,%u %ux%u texture=%ux%u\n",
                __func__, frame->width, frame->height, frame->dst_x,
                frame->dst_y, frame->dst_width, frame->dst_height,
                frame->texture_width, frame->texture_height);
        return false;
    }

    uint64_t required_stride = (uint64_t) frame->width * 4u;
    if (frame->bits_per_pixel != 32 || frame->stride < required_stride ||
        frame->stride % 4u != 0 || required_stride > INT_MAX ||
        frame->stride > INT_MAX) {
        fprintf(stderr,
                WINDOW_LOG_PREFIX
                "%s(): unsupported GL primary CPU layout (%u bpp stride=%u "
                "width=%u)\n",
                __func__, frame->bits_per_pixel, frame->stride, frame->width);
        return false;
    }

    bool full_update = vgpu_display_cpu_payload_is_full_texture_update(frame);
    bool reuse_texture =
        scanout->gl_primary_cpu_valid && scanout->gl_primary_cpu_texture &&
        scanout->gl_primary_cpu_width == frame->texture_width &&
        scanout->gl_primary_cpu_height == frame->texture_height;
    if (!full_update && !reuse_texture) {
        fprintf(stderr,
                "%s(): rejecting partial GL primary CPU update without "
                "retained texture\n",
                __func__);
        return false;
    }

    uint64_t rgba_size = required_stride * frame->height;
    if (rgba_size / frame->height != required_stride || rgba_size > SIZE_MAX) {
        fprintf(stderr, "%s(): primary conversion size overflow\n", __func__);
        return false;
    }

    void *rgba_pixels = SDL_malloc((size_t) rgba_size);
    if (!rgba_pixels) {
        fprintf(stderr, "%s(): failed to allocate primary conversion buffer\n",
                __func__);
        return false;
    }

    if (SDL_ConvertPixels((int) frame->width, (int) frame->height,
                          src_sdl_format, frame->pixels, (int) frame->stride,
                          SDL_PIXELFORMAT_RGBA32, rgba_pixels,
                          (int) required_stride) < 0) {
        fprintf(stderr, "%s(): failed to convert primary pixels: %s\n",
                __func__, SDL_GetError());
        SDL_free(rgba_pixels);
        return false;
    }

    if (SDL_GL_MakeCurrent(scanout->window, scanout->gl_context) < 0) {
        fprintf(stderr, "%s(): failed to make GL context current: %s\n",
                __func__, SDL_GetError());
        SDL_free(rgba_pixels);
        return false;
    }

    GLuint old_texture = scanout->gl_primary_cpu_texture;
    GLuint texture = old_texture;
    bool replace_texture = !reuse_texture;
    if (replace_texture)
        glGenTextures(1, &texture);

    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (replace_texture) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei) frame->texture_width,
                     (GLsizei) frame->texture_height, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, NULL);
    }
    glTexSubImage2D(GL_TEXTURE_2D, 0, (GLint) frame->dst_x,
                    (GLint) frame->dst_y, (GLsizei) frame->width,
                    (GLsizei) frame->height, GL_RGBA, GL_UNSIGNED_BYTE,
                    rgba_pixels);
    GLenum error = glGetError();
    if (error != GL_NO_ERROR) {
        fprintf(stderr,
                "%s(): failed to upload GL primary CPU texture error=0x%x\n",
                __func__, (unsigned) error);
        if (replace_texture && texture)
            glDeleteTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D, 0);
        sdl_scanout_detach_gl_context();
        SDL_free(rgba_pixels);
        return false;
    }

    if (replace_texture && old_texture)
        glDeleteTextures(1, &old_texture);
    glBindTexture(GL_TEXTURE_2D, 0);
    sdl_scanout_detach_gl_context();
    SDL_free(rgba_pixels);

    scanout->gl_primary_cpu_texture = texture;
    scanout->gl_primary_cpu_width = frame->texture_width;
    scanout->gl_primary_cpu_height = frame->texture_height;
    scanout->gl_primary_cpu_valid = true;
    scanout->gl_primary_valid = false;
    return true;
}

static bool sdl_scanout_apply_gl_cursor_frame(
    struct sdl_scanout_info *scanout,
    const struct vgpu_display_payload *payload,
    int32_t x,
    int32_t y,
    uint32_t hot_x,
    uint32_t hot_y)
{
    if (!scanout->gl_context || !payload ||
        payload->type != VGPU_DISPLAY_PAYLOAD_CPU)
        return false;

    const struct vgpu_display_cpu_payload *frame = &payload->cpu;
    SDL_Rect new_cursor_rect = scanout->cursor_rect;
    uint32_t src_sdl_format;

    if (!vgpu_display_cpu_payload_is_full_texture_update(frame)) {
        fprintf(stderr,
                WINDOW_LOG_PREFIX "%s(): rejecting partial GL cursor update\n",
                __func__);
        return false;
    }
    if (frame->width == 0 || frame->height == 0 || frame->width > INT_MAX ||
        frame->height > INT_MAX) {
        fprintf(stderr,
                WINDOW_LOG_PREFIX
                "%s(): cursor size out of GL/SDL range (%ux%u)\n",
                __func__, frame->width, frame->height);
        return false;
    }

    uint64_t required_stride = (uint64_t) frame->width * 4u;
    if (frame->bits_per_pixel != 32 || frame->stride < required_stride ||
        frame->stride % 4u != 0) {
        fprintf(stderr,
                WINDOW_LOG_PREFIX
                "%s(): unsupported cursor layout (%u bpp stride=%u width=%u)\n",
                __func__, frame->bits_per_pixel, frame->stride, frame->width);
        return false;
    }
    if (required_stride > INT_MAX || frame->stride > INT_MAX) {
        fprintf(stderr,
                WINDOW_LOG_PREFIX
                "%s(): cursor pitch out of SDL range (stride=%u row=%" PRIu64
                ")\n",
                __func__, frame->stride, required_stride);
        return false;
    }
    if (!sdl_plane_info_get_sdl_format(&scanout->cursor_plane, payload,
                                       &src_sdl_format))
        return false;
    if (!sdl_cursor_rect_update_position(&new_cursor_rect, x, y, hot_x, hot_y))
        return false;

    uint64_t rgba_size = required_stride * frame->height;
    if (rgba_size / frame->height != required_stride || rgba_size > SIZE_MAX) {
        fprintf(stderr, "%s(): cursor conversion size overflow\n", __func__);
        return false;
    }

    void *rgba_pixels = SDL_malloc((size_t) rgba_size);
    if (!rgba_pixels) {
        fprintf(stderr, "%s(): failed to allocate cursor conversion buffer\n",
                __func__);
        return false;
    }

    if (SDL_ConvertPixels((int) frame->width, (int) frame->height,
                          src_sdl_format, frame->pixels, (int) frame->stride,
                          SDL_PIXELFORMAT_RGBA32, rgba_pixels,
                          (int) required_stride) < 0) {
        fprintf(stderr, "%s(): failed to convert cursor pixels: %s\n", __func__,
                SDL_GetError());
        SDL_free(rgba_pixels);
        return false;
    }

    if (SDL_GL_MakeCurrent(scanout->window, scanout->gl_context) < 0) {
        fprintf(stderr, "%s(): failed to make GL context current: %s\n",
                __func__, SDL_GetError());
        SDL_free(rgba_pixels);
        return false;
    }

    if (!scanout->gl_cursor_texture)
        glGenTextures(1, &scanout->gl_cursor_texture);

    glBindTexture(GL_TEXTURE_2D, scanout->gl_cursor_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, (GLint) frame->width);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei) frame->width,
                 (GLsizei) frame->height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 rgba_pixels);
    GLenum error = glGetError();
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    sdl_scanout_detach_gl_context();
    SDL_free(rgba_pixels);
    if (error != GL_NO_ERROR) {
        fprintf(stderr, "%s(): failed to upload GL cursor texture error=0x%x\n",
                __func__, (unsigned) error);
        return false;
    }

    scanout->cursor_hot_x = hot_x;
    scanout->cursor_hot_y = hot_y;
    new_cursor_rect.w = (int) frame->width;
    new_cursor_rect.h = (int) frame->height;
    scanout->cursor_rect = new_cursor_rect;
    scanout->gl_cursor_width = frame->width;
    scanout->gl_cursor_height = frame->height;
    scanout->gl_cursor_valid = true;

    return true;
}

static bool sdl_scanout_validate_gl_frame(
    const struct vgpu_display_gl_payload *frame)
{
    if (!frame || frame->texture_id == 0 || frame->width == 0 ||
        frame->height == 0 || frame->src_width == 0 || frame->src_height == 0) {
        fprintf(stderr, "%s(): invalid empty VirGL scanout metadata\n",
                __func__);
        return false;
    }

    if (frame->width > INT_MAX || frame->height > INT_MAX ||
        frame->src_x >= frame->width || frame->src_y >= frame->height ||
        frame->src_width > frame->width - frame->src_x ||
        frame->src_height > frame->height - frame->src_y) {
        fprintf(stderr,
                "%s(): invalid VirGL scanout source texture=%ux%u src=%u,%u "
                "%ux%u\n",
                __func__, frame->width, frame->height, frame->src_x,
                frame->src_y, frame->src_width, frame->src_height);
        return false;
    }

    return true;
}

static bool sdl_scanout_apply_gl_frame(
    struct sdl_scanout_info *scanout,
    const struct vgpu_display_gl_payload *frame)
{
    if (!scanout->gl_context || !sdl_scanout_validate_gl_frame(frame))
        return false;

    if (SDL_GL_MakeCurrent(scanout->window, scanout->gl_context) < 0) {
        fprintf(stderr, "%s(): failed to make GL context current: %s\n",
                __func__, SDL_GetError());
        return false;
    }

    if (!scanout->gl_primary_fb)
        glGenFramebuffers(1, &scanout->gl_primary_fb);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, scanout->gl_primary_fb);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, (GLuint) frame->texture_id, 0);
    GLenum status = glCheckFramebufferStatus(GL_READ_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr,
                "%s(): incomplete VirGL scanout framebuffer status=0x%x\n",
                __func__, (unsigned) status);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        sdl_scanout_detach_gl_context();
        return false;
    }

    scanout->gl_primary = *frame;
    scanout->gl_primary_valid = true;
    scanout->gl_primary_cpu_valid = false;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    sdl_scanout_detach_gl_context();
    return true;
}

static void sdl_scanout_render_gl_cpu_primary(struct sdl_scanout_info *scanout,
                                              int window_width,
                                              int window_height)
{
    if (!scanout->gl_primary_cpu_valid || !scanout->gl_primary_cpu_texture)
        return;

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0.0, (GLdouble) window_width, (GLdouble) window_height, 0.0, -1.0,
            1.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    glDisable(GL_BLEND);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, scanout->gl_primary_cpu_texture);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);

    glBegin(GL_QUADS);
    glTexCoord2f(0.0f, 0.0f);
    glVertex2f(0.0f, 0.0f);
    glTexCoord2f(1.0f, 0.0f);
    glVertex2f((GLfloat) window_width, 0.0f);
    glTexCoord2f(1.0f, 1.0f);
    glVertex2f((GLfloat) window_width, (GLfloat) window_height);
    glTexCoord2f(0.0f, 1.0f);
    glVertex2f(0.0f, (GLfloat) window_height);
    glEnd();

    glBindTexture(GL_TEXTURE_2D, 0);
    glDisable(GL_TEXTURE_2D);
}

static void sdl_scanout_render_gl_cursor(struct sdl_scanout_info *scanout,
                                         int window_width,
                                         int window_height)
{
    if (!scanout->gl_cursor_valid || !scanout->gl_cursor_texture ||
        scanout->cursor_rect.w <= 0 || scanout->cursor_rect.h <= 0)
        return;

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0.0, (GLdouble) window_width, (GLdouble) window_height, 0.0, -1.0,
            1.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, scanout->gl_cursor_texture);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);

    GLfloat x0 = (GLfloat) scanout->cursor_rect.x;
    GLfloat y0 = (GLfloat) scanout->cursor_rect.y;
    GLfloat x1 = (GLfloat) (scanout->cursor_rect.x + scanout->cursor_rect.w);
    GLfloat y1 = (GLfloat) (scanout->cursor_rect.y + scanout->cursor_rect.h);

    glBegin(GL_QUADS);
    glTexCoord2f(0.0f, 0.0f);
    glVertex2f(x0, y0);
    glTexCoord2f(1.0f, 0.0f);
    glVertex2f(x1, y0);
    glTexCoord2f(1.0f, 1.0f);
    glVertex2f(x1, y1);
    glTexCoord2f(0.0f, 1.0f);
    glVertex2f(x0, y1);
    glEnd();

    glBindTexture(GL_TEXTURE_2D, 0);
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_BLEND);
}

static void sdl_scanout_render_gl(struct sdl_scanout_info *scanout)
{
    if (!scanout->gl_context)
        return;

    if (SDL_GL_MakeCurrent(scanout->window, scanout->gl_context) < 0)
        return;

    int width = 0;
    int height = 0;
    SDL_GetWindowSize(scanout->window, &width, &height);
    if (width <= 0 || height <= 0) {
        sdl_scanout_detach_gl_context();
        return;
    }

    glViewport(0, 0, width, height);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    if (scanout->gl_primary_valid) {
        const struct vgpu_display_gl_payload *frame = &scanout->gl_primary;
        GLint src_x0 = (GLint) frame->src_x;
        GLint src_x1 = (GLint) (frame->src_x + frame->src_width);
        GLint src_y0 = (GLint) frame->src_y;
        GLint src_y1 = (GLint) (frame->src_y + frame->src_height);

        /* Y_0_TOP textures are already scanout-oriented. Normal GL-origin
         * resources need a reversed read rectangle when blitted into the SDL
         * window framebuffer.
         */
        if (!frame->y_0_top) {
            src_y0 = (GLint) (frame->src_y + frame->src_height);
            src_y1 = (GLint) frame->src_y;
        }

        glBindFramebuffer(GL_READ_FRAMEBUFFER, scanout->gl_primary_fb);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glBlitFramebuffer(src_x0, src_y0, src_x1, src_y1, 0, 0, width, height,
                          GL_COLOR_BUFFER_BIT, GL_LINEAR);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    } else if (scanout->gl_primary_cpu_valid) {
        sdl_scanout_render_gl_cpu_primary(scanout, width, height);
    }

    sdl_scanout_render_gl_cursor(scanout, width, height);
    SDL_GL_SwapWindow(scanout->window);
    sdl_scanout_detach_gl_context();
}
#endif

static void sdl_scanout_render(struct sdl_scanout_info *scanout)
{
#if SEMU_HAS(VIRGL)
    if (scanout->gl_context) {
        sdl_scanout_render_gl(scanout);
        return;
    }
#endif

    SDL_RenderClear(scanout->renderer);

    if (scanout->primary_plane.texture)
        SDL_RenderCopy(scanout->renderer, scanout->primary_plane.texture, NULL,
                       NULL);

    if (scanout->cursor_plane.texture)
        SDL_RenderCopy(scanout->renderer, scanout->cursor_plane.texture, NULL,
                       &scanout->cursor_rect);

    SDL_RenderPresent(scanout->renderer);
}

static void window_drain_display_queue(void)
{
    bool dirty_scanouts[VIRTIO_GPU_MAX_SCANOUTS] = {0};
    struct vgpu_display_cmd cmd;

    /* Drain display bridge commands, update only SDL-owned state, then render
     * each affected scanout once. The bridge publishes reliable clear
     * generations and filters stale lossy frame/move queue entries.
     */
    while (vgpu_display_pop_cmd(&cmd)) {
        /* 'scanout_id' was validated by the guest-facing backend before the
         * command entered the display bridge.
         */
        struct sdl_scanout_info *scanout = &sdl_scanouts[cmd.scanout_id];
        if (!sdl_scanout_is_ready(scanout)) {
            vgpu_display_release_cmd(&cmd);
            continue;
        }

        switch (cmd.type) {
        case VGPU_DISPLAY_CMD_PRIMARY_CLEAR:
            sdl_scanout_clear_primary(scanout);
            dirty_scanouts[cmd.scanout_id] = true;
            break;
        case VGPU_DISPLAY_CMD_CURSOR_CLEAR:
            sdl_scanout_clear_cursor(scanout);
            dirty_scanouts[cmd.scanout_id] = true;
            break;
        case VGPU_DISPLAY_CMD_PRIMARY_SET: {
            /* Use '|=' to keep earlier dirty state for this scanout. A failed
             * upload leaves the old texture visible and does not dirty the
             * scanout by itself.
             */
            struct vgpu_display_payload *payload = cmd.u.primary_set.payload;
            if (!payload)
                break;
            if (payload->type == VGPU_DISPLAY_PAYLOAD_CPU) {
#if SEMU_HAS(VIRGL)
                if (scanout->gl_context) {
                    dirty_scanouts[cmd.scanout_id] |=
                        sdl_scanout_apply_gl_cpu_primary_frame(scanout,
                                                               payload);
                    break;
                }
#endif
                dirty_scanouts[cmd.scanout_id] |= sdl_plane_info_update_texture(
                    scanout->renderer, &scanout->primary_plane, payload,
                    "primary");
#if SEMU_HAS(VIRGL)
            } else if (payload->type == VGPU_DISPLAY_PAYLOAD_GL) {
                dirty_scanouts[cmd.scanout_id] |=
                    sdl_scanout_apply_gl_frame(scanout, &payload->gl);
#endif
            } else {
                fprintf(stderr, "%s(): unsupported primary payload type %u\n",
                        __func__, (unsigned) payload->type);
            }
            break;
        }
        case VGPU_DISPLAY_CMD_CURSOR_SET:
            /* Use '|=' to keep earlier dirty state for this scanout. A failed
             * upload leaves the old cursor visible and does not dirty the
             * scanout by itself.
             */
#if SEMU_HAS(VIRGL)
            if (scanout->gl_context) {
                dirty_scanouts[cmd.scanout_id] |=
                    sdl_scanout_apply_gl_cursor_frame(
                        scanout, cmd.u.cursor_set.payload, cmd.u.cursor_set.x,
                        cmd.u.cursor_set.y, cmd.u.cursor_set.hot_x,
                        cmd.u.cursor_set.hot_y);
                break;
            }
#endif
            dirty_scanouts[cmd.scanout_id] |= sdl_scanout_apply_cursor_frame(
                scanout, cmd.u.cursor_set.payload, cmd.u.cursor_set.x,
                cmd.u.cursor_set.y, cmd.u.cursor_set.hot_x,
                cmd.u.cursor_set.hot_y);
            break;
        case VGPU_DISPLAY_CMD_CURSOR_MOVE: {
            int old_cursor_x = scanout->cursor_rect.x;
            int old_cursor_y = scanout->cursor_rect.y;
            if (!sdl_cursor_rect_update_position(
                    &scanout->cursor_rect, cmd.u.cursor_move.x,
                    cmd.u.cursor_move.y, scanout->cursor_hot_x,
                    scanout->cursor_hot_y))
                break;
            if (old_cursor_x == scanout->cursor_rect.x &&
                old_cursor_y == scanout->cursor_rect.y)
                break;
#if SEMU_HAS(VIRGL)
            if (scanout->gl_context && !scanout->gl_cursor_valid)
                break;
#endif
            dirty_scanouts[cmd.scanout_id] = true;
            break;
        }
        }

        vgpu_display_release_cmd(&cmd);
    }

    for (uint32_t i = 0; i < VIRTIO_GPU_MAX_SCANOUTS; i++) {
        if (!dirty_scanouts[i] || !sdl_scanout_is_ready(&sdl_scanouts[i]))
            continue;
        sdl_scanout_render(&sdl_scanouts[i]);
    }
}
#endif

#if SEMU_HAS(VIRGL)
static void window_drain_renderer_queue(void)
{
    struct vgpu_renderer_request request;

    while (vgpu_renderer_pop_request(&request)) {
        vgpu_renderer_debug_note_execute_begin(&request);
        vgpu_virgl_execute_renderer_request(&request);
        vgpu_renderer_debug_note_execute_end();
    }
}
#endif

/* Main loop runs on the main thread */
static void window_main_loop_sw(void)
{
    if (headless_mode) {
        /* Block until the emulator calls 'window_shutdown_sw()', so 'main()'
         * can proceed to 'pthread_join()' rather than stopping the emulator
         * immediately. There is no SDL event loop in this mode, so the main
         * thread just polls the shared close flag.
         */
        while (!window_is_closed_sw())
            usleep(10000);
        return;
    }

    /* relaxed ordering is sufficient: the only consequence of reading a stale
     * false is a few extra loop iterations. Ordering with the emulator thread
     * is provided by 'pthread_join()', not by this flag.
     */
    while (!window_is_closed_sw()) {
#if SEMU_HAS(VIRTIOINPUT)
        if (vinput_handle_events()) {
            /* User closed the window. Set the flag so 'window_shutdown_sw()'
             * (called from the emulator thread) does not race with us, then
             * return normally so 'main()' can 'pthread_join()' the emulator
             * thread and collect its exit code.
             */
            window_shutdown_sw();
            return;
        }
#else
        SDL_Event e;
        /* Without 'virtio-input', there is no SDL event pump to wake on display
         * commands. Use a short timeout so 'VIRTIOGPU'-only builds periodically
         * drain the display bridge; a future SDL user-event bridge could make
         * this fully event-driven.
         */
        if (SDL_WaitEventTimeout(&e, SDL_EVENT_WAIT_TIMEOUT_MS)) {
            uint32_t processed = 0;
            do {
                if (e.type == SDL_QUIT) {
                    window_shutdown_sw();
                    return;
                }
                processed++;
            } while (processed < SDL_EVENT_BURST_LIMIT && SDL_PollEvent(&e));
        }
#endif

#if SEMU_HAS(VIRGL)
        window_drain_renderer_queue();
#endif
#if SEMU_HAS(VIRTIOGPU)
        window_drain_display_queue();
#endif
    }
}

static bool window_init_sw(bool headless, uint32_t width, uint32_t height)
{
#if SEMU_HAS(VIRGL)
    vgpu_renderer_set_wake_frontend(window_wake_backend_sw);
#endif

    if (headless) {
        headless_mode = true;
#if SEMU_HAS(VIRTIOGPU)
        vgpu_display_set_unavailable();
#endif
        return false;
    }

    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr,
                "window_init_sw(): failed to initialize SDL: %s\n"
                "Running in headless mode.\n",
                SDL_GetError());
        headless_mode = true;
#if SEMU_HAS(VIRTIOGPU)
        vgpu_display_set_unavailable();
#endif
        return false;
    }
    sdl_initialized = true;
#if SEMU_HAS(VIRGL)
    vgpu_renderer_set_wake_renderer(window_wake_renderer_sw);
#endif

#if SEMU_HAS(VIRTIOGPU)
    /* The current machine setup registers exactly one scanout before calling
     * 'window_init_sw()', so materialize scanout 0 directly here. If semu grows
     * multiple scanouts later, this can be extended to iterate all registered
     * scanouts or restored to an explicit per-scanout setup path.
     */
    struct sdl_scanout_info *scanout = &sdl_scanouts[0];
#if SEMU_HAS(VIRGL)
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#endif
    scanout->window = SDL_CreateWindow("semu", SDL_WINDOWPOS_UNDEFINED,
                                       SDL_WINDOWPOS_UNDEFINED, width, height,
                                       SDL_WINDOW_SHOWN
#if SEMU_HAS(VIRGL)
                                           | SDL_WINDOW_OPENGL
#endif
    );
    if (!scanout->window) {
        fprintf(stderr,
                "window_init_sw(): failed to create SDL window for display "
                "0: %s\n"
                "Running in headless mode.\n",
                SDL_GetError());
        headless_mode = true;
        SDL_Quit();
        sdl_initialized = false;
        vgpu_display_set_unavailable();
        return false;
    }

#if SEMU_HAS(VIRGL)
    scanout->gl_context = SDL_GL_CreateContext(scanout->window);
    if (!scanout->gl_context ||
        SDL_GL_MakeCurrent(scanout->window, scanout->gl_context) < 0) {
        fprintf(stderr,
                "window_init_sw(): failed to create GL context for display "
                "0: %s\n"
                "Running in headless mode.\n",
                SDL_GetError());
        if (scanout->gl_context) {
            SDL_GL_DeleteContext(scanout->gl_context);
            scanout->gl_context = NULL;
        }
        SDL_DestroyWindow(scanout->window);
        scanout->window = NULL;
        headless_mode = true;
        SDL_Quit();
        sdl_initialized = false;
        vgpu_display_set_unavailable();
        return false;
    }
    int virgl_ret = vgpu_virgl_init_renderer(scanout);
    if (virgl_ret != 0) {
        fprintf(stderr,
                "window_init_sw(): failed to initialize virglrenderer "
                "(ret=%d)\n"
                "Running in headless mode.\n",
                virgl_ret);
        sdl_scanout_detach_gl_context();
        SDL_GL_DeleteContext(scanout->gl_context);
        scanout->gl_context = NULL;
        SDL_DestroyWindow(scanout->window);
        scanout->window = NULL;
        headless_mode = true;
        SDL_Quit();
        sdl_initialized = false;
        vgpu_display_set_unavailable();
        return false;
    }
    sdl_scanout_detach_gl_context();
#else
    scanout->renderer =
        SDL_CreateRenderer(scanout->window, -1, SDL_RENDERER_ACCELERATED);
    if (!scanout->renderer) {
        fprintf(stderr,
                "window_init_sw(): accelerated renderer not available, "
                "trying software renderer: %s\n",
                SDL_GetError());
        scanout->renderer =
            SDL_CreateRenderer(scanout->window, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!scanout->renderer) {
        fprintf(stderr,
                "window_init_sw(): failed to create renderer for display "
                "0: %s\n"
                "Running in headless mode.\n",
                SDL_GetError());
        SDL_DestroyWindow(scanout->window);
        scanout->window = NULL;
        headless_mode = true;
        SDL_Quit();
        sdl_initialized = false;
        vgpu_display_set_unavailable();
        return false;
    }
#endif

    scanout->window_width = width;
    scanout->window_height = height;
    scanout->cursor_plane.alpha_blend = true;

#if SEMU_HAS(VIRTIOINPUT)
    if (!sdl_input_window)
        sdl_input_window = scanout->window;
#endif

#if SEMU_HAS(VIRGL)
    glViewport(0, 0, (GLsizei) width, (GLsizei) height);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    SDL_GL_SwapWindow(scanout->window);
    sdl_scanout_detach_gl_context();
#else
    SDL_SetRenderDrawColor(scanout->renderer, 0, 0, 0, 255);
    SDL_RenderClear(scanout->renderer);
    SDL_RenderPresent(scanout->renderer);
#endif
#else /* !SEMU_HAS(VIRTIOGPU) */
    sdl_input_window = SDL_CreateWindow("semu", SDL_WINDOWPOS_UNDEFINED,
                                        SDL_WINDOWPOS_UNDEFINED, width, height,
                                        SDL_WINDOW_SHOWN);
    if (!sdl_input_window) {
        fprintf(stderr,
                "window_init_sw(): failed to create SDL window: %s\n"
                "Running in headless mode.\n",
                SDL_GetError());
        headless_mode = true;
        SDL_Quit();
        sdl_initialized = false;
        return false;
    }
#endif
    return true;
}

static void window_cleanup_sw(void)
{
#if SEMU_HAS(VIRGL)
    vgpu_renderer_set_wake_renderer(NULL);
    vgpu_renderer_set_wake_frontend(NULL);
#endif

#if SEMU_HAS(VIRTIOINPUT)
    if (sdl_initialized)
        window_set_mouse_grab_sw(false);
    /* Keep cleanup idempotent when SDL was never initialized or grab release
     * returned early.
     */
    mouse_grabbed = false;
#endif

    wake_write_fd = -1;

#if SEMU_HAS(VIRTIOGPU)
    for (uint32_t i = 0; i < VIRTIO_GPU_MAX_SCANOUTS; i++)
        sdl_scanout_info_cleanup(&sdl_scanouts[i]);

    struct vgpu_display_cmd cmd;
    while (vgpu_display_pop_cmd(&cmd))
        vgpu_display_release_cmd(&cmd);
#elif SEMU_HAS(VIRTIOINPUT)
    if (sdl_input_window)
        SDL_DestroyWindow(sdl_input_window);
#endif

#if SEMU_HAS(VIRTIOINPUT)
    sdl_input_window = NULL;
#endif

    if (sdl_initialized) {
        SDL_Quit();
        sdl_initialized = false;
    }

    /* Cleanup normally runs before process exit. Reset frontend flags anyway
     * so a future re-init path cannot inherit stale headless/shutdown state.
     */
    headless_mode = false;
    should_exit = false;
}

#if SEMU_HAS(VIRGL)
virgl_renderer_gl_context vgpu_window_virgl_create_context(
    int scanout_idx,
    struct virgl_renderer_gl_ctx_param *param)
{
    if (scanout_idx < 0 || scanout_idx >= VIRTIO_GPU_MAX_SCANOUTS)
        return NULL;

    struct sdl_scanout_info *scanout = &sdl_scanouts[scanout_idx];
    if (!scanout->window || !scanout->gl_context)
        return NULL;

    if (SDL_GL_MakeCurrent(scanout->window, scanout->gl_context) < 0)
        return NULL;

    SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);
    if (param) {
        if (param->major_ver)
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, param->major_ver);
        if (param->minor_ver)
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, param->minor_ver);
        if (param->compat_ctx)
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                                SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
    }

    virgl_renderer_gl_context ctx = SDL_GL_CreateContext(scanout->window);
    SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 0);
    sdl_scanout_detach_gl_context();
    return ctx;
}

void vgpu_window_virgl_destroy_context(virgl_renderer_gl_context ctx)
{
    if (ctx)
        SDL_GL_DeleteContext(ctx);
}

int vgpu_window_virgl_make_current(int scanout_idx,
                                   virgl_renderer_gl_context ctx)
{
    if (scanout_idx < 0 || scanout_idx >= VIRTIO_GPU_MAX_SCANOUTS)
        return -1;

    if (!ctx) {
        sdl_scanout_detach_gl_context();
        return 0;
    }

    struct sdl_scanout_info *scanout = &sdl_scanouts[scanout_idx];
    if (!scanout->window)
        return -1;

    return SDL_GL_MakeCurrent(scanout->window, ctx);
}
#endif

const struct window_backend g_window = {
    .window_init = window_init_sw,
    .window_main_loop = window_main_loop_sw,
    .window_shutdown = window_shutdown_sw,
    .window_cleanup = window_cleanup_sw,
    .window_is_closed = window_is_closed_sw,
    .window_set_wake_fd = window_set_wake_fd_sw,
    .window_wake_backend = window_wake_backend_sw,
#if SEMU_HAS(VIRTIOINPUT)
    .window_set_mouse_grab = window_set_mouse_grab_sw,
    .window_is_mouse_grabbed = window_is_mouse_grabbed_sw,
#endif
};
