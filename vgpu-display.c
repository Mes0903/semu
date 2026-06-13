#include <stdlib.h>

#include "vgpu-display.h"

/* 'PRIMARY_SET'/'CURSOR_SET' own CPU-frame snapshots, so each queued command
 * can retain significantly more memory than an input event. Keep this backlog
 * deliberately small: display updates are lossy and quickly become stale, and
 * the emulator thread must be able to apply backpressure rather than accumulate
 * a large queue of old frames.
 */
#define VGPU_DISPLAY_CMD_QUEUE_SIZE 64U
#define VGPU_DISPLAY_CMD_QUEUE_MASK (VGPU_DISPLAY_CMD_QUEUE_SIZE - 1U)

/* Reliable state for plane generations and clear/removal events. The producer
 * advances 'generation' to stale older queued lossy commands. Clear events also
 * publish 'clear_generation', which the SDL consumer mirrors in
 * 'consumed_clear_generation'. Frame payloads remain in the lossy SPSC queue.
 */
struct vgpu_display_plane_state {
    uint32_t generation;
    uint32_t clear_generation;
    uint32_t consumed_clear_generation;
};

static struct vgpu_display_plane_state
    vgpu_display_primary_state[VIRTIO_GPU_MAX_SCANOUTS];
static struct vgpu_display_plane_state
    vgpu_display_cursor_state[VIRTIO_GPU_MAX_SCANOUTS];
static uint32_t vgpu_display_scanout_count = 1U;

/* The SPSC queue carries lossy frame/move commands. It's process-wide and
 * currently assumes one 'virtio-gpu' producer. The GPU backend is the only
 * producer and the window backend is the only consumer. Commands entering this
 * bridge carry 'scanout_id' values already validated by the guest-facing
 * backend; the SDL consumer relies on that internal contract.
 */
static struct vgpu_display_cmd
    vgpu_display_cmd_queue[VGPU_DISPLAY_CMD_QUEUE_SIZE];
static uint32_t vgpu_display_cmd_head;
static uint32_t vgpu_display_cmd_tail;

static bool vgpu_display_unavailable;

static bool vgpu_display_is_cmd_stale(const struct vgpu_display_cmd *cmd)
{
    switch (cmd->type) {
    case VGPU_DISPLAY_CMD_PRIMARY_SET:
        return cmd->generation !=
               __atomic_load_n(
                   &vgpu_display_primary_state[cmd->scanout_id].generation,
                   __ATOMIC_ACQUIRE);
    case VGPU_DISPLAY_CMD_CURSOR_SET:
    case VGPU_DISPLAY_CMD_CURSOR_MOVE:
        return cmd->generation !=
               __atomic_load_n(
                   &vgpu_display_cursor_state[cmd->scanout_id].generation,
                   __ATOMIC_ACQUIRE);
    default:
        return false;
    }
}

static bool vgpu_display_pop_pending_clear_cmd(
    struct vgpu_display_plane_state *states,
    enum vgpu_display_cmd_type type,
    struct vgpu_display_cmd *cmd)
{
    uint32_t scanout_count =
        __atomic_load_n(&vgpu_display_scanout_count, __ATOMIC_ACQUIRE);

    for (uint32_t i = 0; i < scanout_count; i++) {
        struct vgpu_display_plane_state *state = &states[i];
        uint32_t clear_generation =
            __atomic_load_n(&state->clear_generation, __ATOMIC_ACQUIRE);

        if (state->consumed_clear_generation == clear_generation)
            continue;

        state->consumed_clear_generation = clear_generation;

        *cmd = (struct vgpu_display_cmd) {
            .type = type,
            .scanout_id = i,
            .generation = clear_generation,
        };
        return true;
    }

    return false;
}

static uint32_t vgpu_display_advance_plane_generation(
    struct vgpu_display_plane_state *state)
{
    return __atomic_add_fetch(&state->generation, 1U, __ATOMIC_ACQ_REL);
}

static uint32_t vgpu_display_plane_generation(
    const struct vgpu_display_plane_state *state)
{
    return __atomic_load_n(&state->generation, __ATOMIC_ACQUIRE);
}

static enum vgpu_display_publish_result vgpu_display_publish_plane_clear(
    struct vgpu_display_plane_state *state)
{
    if (__atomic_load_n(&vgpu_display_unavailable, __ATOMIC_ACQUIRE))
        return VGPU_DISPLAY_PUBLISH_UNAVAILABLE;

    uint32_t generation = vgpu_display_advance_plane_generation(state);
    __atomic_store_n(&state->clear_generation, generation, __ATOMIC_RELEASE);
    return VGPU_DISPLAY_PUBLISH_OK;
}

void vgpu_display_set_scanout_count(uint32_t scanout_count)
{
    if (scanout_count > VIRTIO_GPU_MAX_SCANOUTS)
        scanout_count = VIRTIO_GPU_MAX_SCANOUTS;

    __atomic_store_n(&vgpu_display_scanout_count, scanout_count,
                     __ATOMIC_RELEASE);
}

uint32_t vgpu_display_primary_generation(uint32_t scanout_id)
{
    return vgpu_display_plane_generation(
        &vgpu_display_primary_state[scanout_id]);
}

uint32_t vgpu_display_advance_primary_generation(uint32_t scanout_id)
{
    return vgpu_display_advance_plane_generation(
        &vgpu_display_primary_state[scanout_id]);
}

enum vgpu_display_publish_result vgpu_display_publish_primary_clear(
    uint32_t scanout_id)
{
    return vgpu_display_publish_plane_clear(
        &vgpu_display_primary_state[scanout_id]);
}

enum vgpu_display_publish_result vgpu_display_publish_cursor_clear(
    uint32_t scanout_id)
{
    return vgpu_display_publish_plane_clear(
        &vgpu_display_cursor_state[scanout_id]);
}

static bool vgpu_display_is_cmd_queue_full(void)
{
    uint32_t head = __atomic_load_n(&vgpu_display_cmd_head, __ATOMIC_RELAXED);
    uint32_t tail = __atomic_load_n(&vgpu_display_cmd_tail, __ATOMIC_ACQUIRE);
    uint32_t next = (head + 1U) & VGPU_DISPLAY_CMD_QUEUE_MASK;
    return next == tail;
}

static enum vgpu_display_publish_result vgpu_display_push_cmd(
    const struct vgpu_display_cmd *cmd)
{
    uint32_t head = __atomic_load_n(&vgpu_display_cmd_head, __ATOMIC_RELAXED);
    uint32_t tail = __atomic_load_n(&vgpu_display_cmd_tail, __ATOMIC_ACQUIRE);
    uint32_t next = (head + 1U) & VGPU_DISPLAY_CMD_QUEUE_MASK;

    if (next == tail)
        return VGPU_DISPLAY_PUBLISH_QUEUE_FULL;

    vgpu_display_cmd_queue[head] = *cmd;
    __atomic_store_n(&vgpu_display_cmd_head, next, __ATOMIC_RELEASE);
    return VGPU_DISPLAY_PUBLISH_OK;
}

static bool vgpu_display_pop_queued_cmd(struct vgpu_display_cmd *cmd)
{
    uint32_t tail = __atomic_load_n(&vgpu_display_cmd_tail, __ATOMIC_RELAXED);
    uint32_t head = __atomic_load_n(&vgpu_display_cmd_head, __ATOMIC_ACQUIRE);

    if (tail == head)
        return false;

    *cmd = vgpu_display_cmd_queue[tail];
    __atomic_store_n(&vgpu_display_cmd_tail,
                     (tail + 1U) & VGPU_DISPLAY_CMD_QUEUE_MASK,
                     __ATOMIC_RELEASE);
    return true;
}

void vgpu_display_release_cmd(struct vgpu_display_cmd *cmd)
{
    switch (cmd->type) {
    case VGPU_DISPLAY_CMD_PRIMARY_SET:
        free(cmd->u.primary_set.payload);
        break;
    case VGPU_DISPLAY_CMD_CURSOR_SET:
        free(cmd->u.cursor_set.payload);
        break;
    default:
        break;
    }
}

bool vgpu_display_pop_cmd(struct vgpu_display_cmd *cmd)
{
    /* Return true when '*cmd' is filled with a clear command or a valid queued
     * frame/move command. Stale queued commands are released and skipped;
     * return false only when no command remains.
     */
    for (;;) {
        if (vgpu_display_pop_pending_clear_cmd(vgpu_display_primary_state,
                                               VGPU_DISPLAY_CMD_PRIMARY_CLEAR,
                                               cmd))
            return true;
        if (vgpu_display_pop_pending_clear_cmd(
                vgpu_display_cursor_state, VGPU_DISPLAY_CMD_CURSOR_CLEAR, cmd))
            return true;

        if (!vgpu_display_pop_queued_cmd(cmd))
            return false;
        if (!vgpu_display_is_cmd_stale(cmd))
            return true;

        vgpu_display_release_cmd(cmd);
    }
}

static void vgpu_display_drain_and_release_cmds(void)
{
    struct vgpu_display_cmd cmd;

    while (vgpu_display_pop_cmd(&cmd))
        vgpu_display_release_cmd(&cmd);
}

void vgpu_display_set_unavailable(void)
{
    /* This is an init-only fallback path for 'window-sw' initialization
     * failure, before the emulator thread starts publishing display commands.
     * It is not a concurrent shutdown primitive: a producer could otherwise
     * observe 'vgpu_display_unavailable == false', race with this drain, and
     * enqueue a payload after the queue was already drained.
     *
     * Still publish the latch atomically so later call sites keep the same
     * one-way handoff rule.
     */
    __atomic_store_n(&vgpu_display_unavailable, true, __ATOMIC_RELEASE);
    vgpu_display_drain_and_release_cmds();
}

void vgpu_display_shutdown_after_producer_stopped(void)
{
    __atomic_store_n(&vgpu_display_unavailable, true, __ATOMIC_RELEASE);
    vgpu_display_drain_and_release_cmds();
}

bool vgpu_display_can_publish(void)
{
    return !__atomic_load_n(&vgpu_display_unavailable, __ATOMIC_ACQUIRE) &&
           !vgpu_display_is_cmd_queue_full();
}


bool vgpu_display_cpu_payload_is_full_texture_update(
    const struct vgpu_display_cpu_payload *payload)
{
    return payload && payload->texture_width != 0 &&
           payload->texture_height != 0 && payload->dst_x == 0 &&
           payload->dst_y == 0 && payload->width == payload->texture_width &&
           payload->height == payload->texture_height &&
           payload->dst_width == payload->texture_width &&
           payload->dst_height == payload->texture_height;
}

enum vgpu_display_publish_result vgpu_display_publish_primary_set(
    uint32_t scanout_id,
    struct vgpu_display_payload *payload)
{
    if (__atomic_load_n(&vgpu_display_unavailable, __ATOMIC_ACQUIRE))
        return VGPU_DISPLAY_PUBLISH_UNAVAILABLE;

    struct vgpu_display_cmd cmd = {
        .type = VGPU_DISPLAY_CMD_PRIMARY_SET,
        .scanout_id = scanout_id,
        .generation = vgpu_display_primary_generation(scanout_id),
        .u.primary_set = {.payload = payload},
    };
    return vgpu_display_push_cmd(&cmd);
}

enum vgpu_display_publish_result
vgpu_display_publish_primary_set_next_generation(
    uint32_t scanout_id,
    struct vgpu_display_payload *payload)
{
    if (__atomic_load_n(&vgpu_display_unavailable, __ATOMIC_ACQUIRE))
        return VGPU_DISPLAY_PUBLISH_UNAVAILABLE;
    if (vgpu_display_is_cmd_queue_full())
        return VGPU_DISPLAY_PUBLISH_QUEUE_FULL;

    uint32_t generation = vgpu_display_advance_plane_generation(
        &vgpu_display_primary_state[scanout_id]);
    struct vgpu_display_cmd cmd = {
        .type = VGPU_DISPLAY_CMD_PRIMARY_SET,
        .scanout_id = scanout_id,
        .generation = generation,
        .u.primary_set = {.payload = payload},
    };
    return vgpu_display_push_cmd(&cmd);
}

enum vgpu_display_publish_result vgpu_display_publish_cursor_set(
    uint32_t scanout_id,
    struct vgpu_display_payload *payload,
    int32_t x,
    int32_t y,
    uint32_t hot_x,
    uint32_t hot_y)
{
    if (__atomic_load_n(&vgpu_display_unavailable, __ATOMIC_ACQUIRE))
        return VGPU_DISPLAY_PUBLISH_UNAVAILABLE;

    struct vgpu_display_cmd cmd = {
        .type = VGPU_DISPLAY_CMD_CURSOR_SET,
        .scanout_id = scanout_id,
        .generation = vgpu_display_plane_generation(
            &vgpu_display_cursor_state[scanout_id]),
        .u.cursor_set =
            {
                .payload = payload,
                .x = x,
                .y = y,
                .hot_x = hot_x,
                .hot_y = hot_y,
            },
    };
    return vgpu_display_push_cmd(&cmd);
}

enum vgpu_display_publish_result
vgpu_display_publish_cursor_move(uint32_t scanout_id, int32_t x, int32_t y)
{
    if (__atomic_load_n(&vgpu_display_unavailable, __ATOMIC_ACQUIRE))
        return VGPU_DISPLAY_PUBLISH_UNAVAILABLE;

    struct vgpu_display_cmd cmd = {
        .type = VGPU_DISPLAY_CMD_CURSOR_MOVE,
        .scanout_id = scanout_id,
        .generation = vgpu_display_plane_generation(
            &vgpu_display_cursor_state[scanout_id]),
        .u.cursor_move = {.x = x, .y = y},
    };
    return vgpu_display_push_cmd(&cmd);
}
