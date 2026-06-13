#include "vgpu-renderer.h"

#include <pthread.h>
#include <string.h>

struct vgpu_renderer_ring_state {
    uint32_t head;
    uint32_t tail;
    uint32_t count;
};

static pthread_mutex_t vgpu_renderer_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t vgpu_renderer_reset_cond = PTHREAD_COND_INITIALIZER;
static bool vgpu_renderer_resetting;
static bool vgpu_renderer_reset_owner_active;
static pthread_t vgpu_renderer_reset_owner_thread;
static bool vgpu_renderer_available;
static uint64_t vgpu_renderer_active_generation;
static void (*vgpu_renderer_wake_renderer)(void);
static void (*vgpu_renderer_wake_frontend)(void);

static struct vgpu_renderer_request
    vgpu_renderer_requests[VGPU_RENDERER_QUEUE_CAPACITY];
static struct vgpu_renderer_ring_state vgpu_renderer_request_ring;

static struct vgpu_renderer_completion
    vgpu_renderer_completions[VGPU_RENDERER_QUEUE_CAPACITY];
static struct vgpu_renderer_ring_state vgpu_renderer_completion_ring;

static uint64_t debug_requests_submitted;
static uint64_t debug_requests_dropped;
static uint64_t debug_requests_popped;
static uint64_t debug_completions_submitted;
static uint64_t debug_completions_dropped;
static uint64_t debug_completions_popped;
static uint64_t debug_queue_resets;
static uint64_t debug_execute_started;
static uint64_t debug_execute_finished;
static uint64_t debug_current_execute_seq;
static enum vgpu_renderer_request_type debug_current_request_type;
static uint32_t debug_current_command_type;
static uint32_t debug_current_token_id;
static uint64_t debug_current_generation;

static uint32_t vgpu_renderer_ring_next(uint32_t index)
{
    return (index + 1U) % VGPU_RENDERER_QUEUE_CAPACITY;
}

static bool vgpu_renderer_ring_full(const struct vgpu_renderer_ring_state *ring)
{
    return ring->count == VGPU_RENDERER_QUEUE_CAPACITY;
}

static bool vgpu_renderer_ring_empty(
    const struct vgpu_renderer_ring_state *ring)
{
    return ring->count == 0;
}

static void vgpu_renderer_ring_reset(struct vgpu_renderer_ring_state *ring)
{
    memset(ring, 0, sizeof(*ring));
}

static void vgpu_renderer_release_request(
    const struct vgpu_renderer_request *request)
{
    if (request->release_payload)
        request->release_payload(request->payload);
}

static void vgpu_renderer_release_completion(
    const struct vgpu_renderer_completion *completion)
{
    if (completion->response && completion->release_response)
        completion->release_response(completion->response);
}

static void vgpu_renderer_copy_and_clear_requests(
    struct vgpu_renderer_request *out,
    uint32_t *out_count)
{
    *out_count = 0;
    while (!vgpu_renderer_ring_empty(&vgpu_renderer_request_ring)) {
        out[*out_count] =
            vgpu_renderer_requests[vgpu_renderer_request_ring.tail];
        (*out_count)++;
        vgpu_renderer_request_ring.tail =
            vgpu_renderer_ring_next(vgpu_renderer_request_ring.tail);
        vgpu_renderer_request_ring.count--;
    }
    vgpu_renderer_ring_reset(&vgpu_renderer_request_ring);
}

static void vgpu_renderer_copy_and_clear_completions(
    struct vgpu_renderer_completion *out,
    uint32_t *out_count)
{
    *out_count = 0;
    while (!vgpu_renderer_ring_empty(&vgpu_renderer_completion_ring)) {
        out[*out_count] =
            vgpu_renderer_completions[vgpu_renderer_completion_ring.tail];
        (*out_count)++;
        vgpu_renderer_completion_ring.tail =
            vgpu_renderer_ring_next(vgpu_renderer_completion_ring.tail);
        vgpu_renderer_completion_ring.count--;
    }
    vgpu_renderer_ring_reset(&vgpu_renderer_completion_ring);
}

void vgpu_renderer_set_wake_renderer(void (*wake_renderer)(void))
{
    pthread_mutex_lock(&vgpu_renderer_lock);
    vgpu_renderer_wake_renderer = wake_renderer;
    pthread_mutex_unlock(&vgpu_renderer_lock);
}

void vgpu_renderer_set_wake_frontend(void (*wake_frontend)(void))
{
    pthread_mutex_lock(&vgpu_renderer_lock);
    vgpu_renderer_wake_frontend = wake_frontend;
    pthread_mutex_unlock(&vgpu_renderer_lock);
}

bool vgpu_renderer_submit(const struct vgpu_renderer_request *request)
{
    void (*wake_renderer)(void) = NULL;
    bool submitted = false;

    if (!request)
        return false;

    pthread_mutex_lock(&vgpu_renderer_lock);
    if (!vgpu_renderer_available || vgpu_renderer_resetting ||
        vgpu_renderer_ring_full(&vgpu_renderer_request_ring)) {
        debug_requests_dropped++;
        goto out;
    }

    vgpu_renderer_requests[vgpu_renderer_request_ring.head] = *request;
    vgpu_renderer_request_ring.head =
        vgpu_renderer_ring_next(vgpu_renderer_request_ring.head);
    vgpu_renderer_request_ring.count++;
    debug_requests_submitted++;
    wake_renderer = vgpu_renderer_wake_renderer;
    submitted = true;

out:
    pthread_mutex_unlock(&vgpu_renderer_lock);
    if (submitted && wake_renderer)
        wake_renderer();
    return submitted;
}

bool vgpu_renderer_pop_request(struct vgpu_renderer_request *request)
{
    bool popped = false;

    if (!request)
        return false;

    pthread_mutex_lock(&vgpu_renderer_lock);
    if (!vgpu_renderer_available || vgpu_renderer_resetting ||
        vgpu_renderer_ring_empty(&vgpu_renderer_request_ring))
        goto out;

    *request = vgpu_renderer_requests[vgpu_renderer_request_ring.tail];
    vgpu_renderer_request_ring.tail =
        vgpu_renderer_ring_next(vgpu_renderer_request_ring.tail);
    vgpu_renderer_request_ring.count--;
    debug_requests_popped++;
    popped = true;

out:
    pthread_mutex_unlock(&vgpu_renderer_lock);
    return popped;
}

bool vgpu_renderer_complete(const struct vgpu_renderer_completion *completion)
{
    struct vgpu_renderer_completion rejected = {0};
    void (*wake_frontend)(void) = NULL;
    bool release_rejected = false;
    bool completed = false;

    if (!completion)
        return false;

    pthread_mutex_lock(&vgpu_renderer_lock);
    if (!vgpu_renderer_available || vgpu_renderer_resetting ||
        completion->token.generation != vgpu_renderer_active_generation ||
        vgpu_renderer_ring_full(&vgpu_renderer_completion_ring)) {
        rejected = *completion;
        release_rejected = true;
        debug_completions_dropped++;
        goto out;
    }

    vgpu_renderer_completions[vgpu_renderer_completion_ring.head] = *completion;
    vgpu_renderer_completion_ring.head =
        vgpu_renderer_ring_next(vgpu_renderer_completion_ring.head);
    vgpu_renderer_completion_ring.count++;
    debug_completions_submitted++;
    wake_frontend = vgpu_renderer_wake_frontend;
    completed = true;

out:
    pthread_mutex_unlock(&vgpu_renderer_lock);
    if (release_rejected)
        vgpu_renderer_release_completion(&rejected);
    if (completed && wake_frontend)
        wake_frontend();
    return completed;
}

bool vgpu_renderer_pop_completion(struct vgpu_renderer_completion *completion)
{
    bool popped = false;

    if (!completion)
        return false;

    pthread_mutex_lock(&vgpu_renderer_lock);
    if (!vgpu_renderer_available || vgpu_renderer_resetting ||
        vgpu_renderer_ring_empty(&vgpu_renderer_completion_ring))
        goto out;

    *completion = vgpu_renderer_completions[vgpu_renderer_completion_ring.tail];
    vgpu_renderer_completion_ring.tail =
        vgpu_renderer_ring_next(vgpu_renderer_completion_ring.tail);
    vgpu_renderer_completion_ring.count--;
    debug_completions_popped++;
    popped = true;

out:
    pthread_mutex_unlock(&vgpu_renderer_lock);
    return popped;
}

static void vgpu_renderer_transition_queues(uint64_t generation, bool activate)
{
    struct vgpu_renderer_request requests[VGPU_RENDERER_QUEUE_CAPACITY];
    struct vgpu_renderer_completion completions[VGPU_RENDERER_QUEUE_CAPACITY];
    uint32_t request_count = 0;
    uint32_t completion_count = 0;

    pthread_mutex_lock(&vgpu_renderer_lock);
    pthread_t self = pthread_self();
    if (vgpu_renderer_reset_owner_active &&
        pthread_equal(vgpu_renderer_reset_owner_thread, self)) {
        pthread_mutex_unlock(&vgpu_renderer_lock);
        return;
    }
    while (vgpu_renderer_reset_owner_active)
        pthread_cond_wait(&vgpu_renderer_reset_cond, &vgpu_renderer_lock);
    vgpu_renderer_reset_owner_active = true;
    vgpu_renderer_reset_owner_thread = self;
    vgpu_renderer_resetting = true;
    if (!activate)
        vgpu_renderer_available = false;
    vgpu_renderer_copy_and_clear_requests(requests, &request_count);
    vgpu_renderer_copy_and_clear_completions(completions, &completion_count);
    pthread_mutex_unlock(&vgpu_renderer_lock);

    for (uint32_t i = 0; i < request_count; i++)
        vgpu_renderer_release_request(&requests[i]);
    for (uint32_t i = 0; i < completion_count; i++)
        vgpu_renderer_release_completion(&completions[i]);

    pthread_mutex_lock(&vgpu_renderer_lock);
    vgpu_renderer_active_generation = generation;
    if (activate) {
        vgpu_renderer_available = true;
        debug_queue_resets++;
    }
    vgpu_renderer_resetting = false;
    vgpu_renderer_reset_owner_active = false;
    pthread_cond_broadcast(&vgpu_renderer_reset_cond);
    pthread_mutex_unlock(&vgpu_renderer_lock);
}

void vgpu_renderer_init_queues(uint64_t generation)
{
    vgpu_renderer_transition_queues(generation, true);
}

void vgpu_renderer_reset_queues(uint64_t generation)
{
    vgpu_renderer_transition_queues(generation, true);
}

void vgpu_renderer_shutdown_queues(void)
{
    vgpu_renderer_transition_queues(0, false);
}

void vgpu_renderer_debug_note_execute_begin(
    const struct vgpu_renderer_request *request)
{
    uint64_t seq;

    if (!request)
        return;

    pthread_mutex_lock(&vgpu_renderer_lock);
    seq = ++debug_execute_started;
    debug_current_request_type = request->type;
    debug_current_command_type = request->command_type;
    debug_current_token_id = request->token.id;
    debug_current_generation = request->token.generation;
    debug_current_execute_seq = seq;
    pthread_mutex_unlock(&vgpu_renderer_lock);
}

void vgpu_renderer_debug_note_execute_end(void)
{
    pthread_mutex_lock(&vgpu_renderer_lock);
    debug_execute_finished++;
    debug_current_execute_seq = 0;
    pthread_mutex_unlock(&vgpu_renderer_lock);
}

void vgpu_renderer_debug_snapshot(struct vgpu_renderer_debug_stats *stats)
{
    if (!stats)
        return;

    pthread_mutex_lock(&vgpu_renderer_lock);
    *stats = (struct vgpu_renderer_debug_stats) {
        .active_generation = vgpu_renderer_active_generation,
        .request_head = vgpu_renderer_request_ring.head,
        .request_tail = vgpu_renderer_request_ring.tail,
        .request_depth = vgpu_renderer_request_ring.count,
        .completion_head = vgpu_renderer_completion_ring.head,
        .completion_tail = vgpu_renderer_completion_ring.tail,
        .completion_depth = vgpu_renderer_completion_ring.count,
        .available = vgpu_renderer_available,
        .resetting = vgpu_renderer_resetting,
        .requests_submitted = debug_requests_submitted,
        .requests_dropped = debug_requests_dropped,
        .requests_popped = debug_requests_popped,
        .completions_submitted = debug_completions_submitted,
        .completions_dropped = debug_completions_dropped,
        .completions_popped = debug_completions_popped,
        .queue_resets = debug_queue_resets,
        .execute_started = debug_execute_started,
        .execute_finished = debug_execute_finished,
        .current_execute_seq = debug_current_execute_seq,
        .current_request_type = debug_current_request_type,
        .current_command_type = debug_current_command_type,
        .current_token_id = debug_current_token_id,
        .current_generation = debug_current_generation,
    };
    pthread_mutex_unlock(&vgpu_renderer_lock);
}
