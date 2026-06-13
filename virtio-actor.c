#include "virtio-actor.h"

#include <errno.h>
#include <limits.h>
#include <string.h>

static bool virtio_actor_state_valid(enum virtio_actor_state state)
{
    return state >= VIRTIO_ACTOR_CREATED && state <= VIRTIO_ACTOR_FAILED;
}

static struct virtio_actor *virtio_actor_mutable(
    const struct virtio_actor *actor)
{
    return (struct virtio_actor *) actor;
}

static bool virtio_actor_can_configure(enum virtio_actor_state state)
{
    return state == VIRTIO_ACTOR_CREATED || state == VIRTIO_ACTOR_PAUSED ||
           state == VIRTIO_ACTOR_RESETTING;
}

static bool virtio_actor_can_activate(enum virtio_actor_state state)
{
    return state == VIRTIO_ACTOR_CONFIGURING;
}

static bool virtio_actor_terminal(enum virtio_actor_state state)
{
    return state == VIRTIO_ACTOR_STOPPING || state == VIRTIO_ACTOR_STOPPED ||
           state == VIRTIO_ACTOR_FAILED;
}

static int virtio_actor_wait_terminal_error(enum virtio_actor_state state,
                                            enum virtio_actor_state target)
{
    if (state == target)
        return 0;
    if (state == VIRTIO_ACTOR_FAILED)
        return -EIO;
    if (state == VIRTIO_ACTOR_STOPPED)
        return -ECANCELED;
    if (state == VIRTIO_ACTOR_STOPPING && target != VIRTIO_ACTOR_STOPPED)
        return -ECANCELED;
    return 0;
}

static int virtio_actor_broadcast_unlock(struct virtio_actor *actor)
{
    int ret = pthread_cond_broadcast(&actor->cond);
    int unlock_ret = pthread_mutex_unlock(&actor->lock);

    if (ret != 0)
        return -ret;
    if (unlock_ret != 0)
        return -unlock_ret;
    return 0;
}

static int virtio_actor_lock(struct virtio_actor *actor)
{
    int ret = pthread_mutex_lock(&actor->lock);

    if (ret != 0)
        return -ret;
    return 0;
}

static int virtio_actor_unlock(struct virtio_actor *actor)
{
    int ret = pthread_mutex_unlock(&actor->lock);

    if (ret != 0)
        return -ret;
    return 0;
}

static bool virtio_actor_claim_backend(struct virtio_actor *actor,
                                       uint64_t generation)
{
    bool claimed = false;

    pthread_mutex_lock(&actor->lock);
    if (actor->state == VIRTIO_ACTOR_ACTIVE &&
        actor->generation == generation) {
        actor->inside_backend_mutation = true;
        claimed = true;
    }
    pthread_mutex_unlock(&actor->lock);
    return claimed;
}

static void virtio_actor_release_backend(struct virtio_actor *actor)
{
    pthread_mutex_lock(&actor->lock);
    actor->inside_backend_mutation = false;
    pthread_cond_broadcast(&actor->cond);
    pthread_mutex_unlock(&actor->lock);
}

static int virtio_actor_call_drain(struct virtio_actor *actor,
                                   uint16_t queue_index,
                                   uint64_t generation,
                                   bool *called)
{
    int ret = 0;

    *called = false;
    if (!actor->ops || !actor->ops->drain_queue)
        return 0;
    if (!virtio_actor_claim_backend(actor, generation))
        return 0;

    *called = true;
    ret =
        actor->ops->drain_queue(actor->opaque, actor, queue_index, generation);
    virtio_actor_release_backend(actor);
    return ret;
}

static bool virtio_actor_call_has_work(struct virtio_actor *actor,
                                       uint16_t queue_index,
                                       uint64_t generation,
                                       bool *called)
{
    bool has_work = false;

    *called = false;
    if (!actor->ops || !actor->ops->queue_has_work)
        return false;
    if (!virtio_actor_claim_backend(actor, generation))
        return false;

    *called = true;
    has_work = actor->ops->queue_has_work(actor->opaque, actor, queue_index,
                                          generation);
    virtio_actor_release_backend(actor);
    return has_work;
}

static int virtio_actor_enter_stopped_locked(struct virtio_actor *actor)
{
    actor->state = VIRTIO_ACTOR_STOPPED;
    actor->stop_requested = true;
    actor->wake_pending = true;
    atomic_store_explicit(&actor->queue_pending_bits, 0, memory_order_release);
    return pthread_cond_broadcast(&actor->cond) == 0 ? 0 : -EINVAL;
}

static int virtio_actor_handle_control_locked(struct virtio_actor *actor,
                                              bool *exit_thread)
{
    if (actor->state == VIRTIO_ACTOR_STOPPING) {
        int ret = virtio_actor_enter_stopped_locked(actor);
        *exit_thread = true;
        return ret;
    }

    if (actor->state == VIRTIO_ACTOR_FAILED) {
        *exit_thread = true;
        return 0;
    }

    if (actor->state == VIRTIO_ACTOR_PAUSING &&
        !actor->inside_backend_mutation) {
        actor->state = VIRTIO_ACTOR_PAUSED;
        int ret = pthread_cond_broadcast(&actor->cond);
        if (ret != 0)
            return -ret;
    }

    *exit_thread = false;
    return 0;
}

static int virtio_actor_process_queue(struct virtio_actor *actor,
                                      uint16_t queue_index,
                                      uint64_t generation)
{
    unsigned mask = 1u << queue_index;

    /*
     * Later virtq integration will make drain_queue() pop until no
     * descriptor remains and queue_has_work() reread avail idx after this
     * release clear. That double-check is what prevents a lost wake when a
     * vCPU notifies while the actor is acknowledging the pending bit.
     */
    for (;;) {
        pthread_mutex_lock(&actor->lock);
        bool stale = actor->generation != generation ||
                     actor->state != VIRTIO_ACTOR_ACTIVE;
        pthread_mutex_unlock(&actor->lock);
        if (stale)
            return 0;

        bool drain_called;
        int ret = virtio_actor_call_drain(actor, queue_index, generation,
                                          &drain_called);
        if (ret < 0)
            return ret;
        if (!drain_called)
            return 0;

        atomic_fetch_and_explicit(&actor->queue_pending_bits, ~mask,
                                  memory_order_release);

        pthread_mutex_lock(&actor->lock);
        stale = actor->generation != generation ||
                actor->state != VIRTIO_ACTOR_ACTIVE;
        pthread_mutex_unlock(&actor->lock);
        if (stale)
            return 0;

        bool has_work_called;
        if (!virtio_actor_call_has_work(actor, queue_index, generation,
                                        &has_work_called))
            return 0;
        if (!has_work_called)
            return 0;

        atomic_fetch_or_explicit(&actor->queue_pending_bits, mask,
                                 memory_order_acq_rel);
    }
}

static int virtio_actor_process_pending(struct virtio_actor *actor)
{
    uint64_t generation;
    unsigned pending;

    pthread_mutex_lock(&actor->lock);
    generation = actor->generation;
    pthread_mutex_unlock(&actor->lock);

    pending =
        atomic_load_explicit(&actor->queue_pending_bits, memory_order_acquire);
    for (uint16_t queue = 0; queue < actor->queue_count; queue++) {
        if ((pending & (1u << queue)) == 0)
            continue;
        int ret = virtio_actor_process_queue(actor, queue, generation);
        if (ret < 0)
            return ret;
    }

    return 0;
}

static void *virtio_actor_thread_main(void *opaque)
{
    struct virtio_actor *actor = opaque;

    for (;;) {
        bool exit_thread;
        int ret;

        pthread_mutex_lock(&actor->lock);
        for (;;) {
            ret = virtio_actor_handle_control_locked(actor, &exit_thread);
            if (ret < 0) {
                pthread_mutex_unlock(&actor->lock);
                virtio_actor_fail(actor);
                return NULL;
            }
            if (exit_thread) {
                pthread_mutex_unlock(&actor->lock);
                return NULL;
            }
            if (actor->state == VIRTIO_ACTOR_ACTIVE &&
                atomic_load_explicit(&actor->queue_pending_bits,
                                     memory_order_acquire) != 0)
                break;

            actor->wake_pending = false;
            ret = pthread_cond_wait(&actor->cond, &actor->lock);
            if (ret != 0) {
                pthread_mutex_unlock(&actor->lock);
                virtio_actor_fail(actor);
                return NULL;
            }
        }
        pthread_mutex_unlock(&actor->lock);

        virtio_actor_ack_wake(actor);
        ret = virtio_actor_process_pending(actor);
        if (ret < 0) {
            virtio_actor_fail(actor);
            return NULL;
        }
    }
}

int virtio_actor_init(struct virtio_actor *actor,
                      const struct virtio_actor_ops *ops,
                      void *opaque,
                      uint16_t queue_count)
{
    int ret;

    if (!actor || queue_count == 0 || queue_count > sizeof(unsigned) * CHAR_BIT)
        return -EINVAL;

    memset(actor, 0, sizeof(*actor));
    actor->wake_notifier.read_fd = -1;
    actor->wake_notifier.write_fd = -1;

    ret = pthread_mutex_init(&actor->lock, NULL);
    if (ret != 0)
        return -ret;
    ret = pthread_cond_init(&actor->cond, NULL);
    if (ret != 0) {
        pthread_mutex_destroy(&actor->lock);
        return -ret;
    }
    ret = semu_event_notifier_init(&actor->wake_notifier);
    if (ret < 0) {
        pthread_cond_destroy(&actor->cond);
        pthread_mutex_destroy(&actor->lock);
        return ret;
    }

    actor->state = VIRTIO_ACTOR_CREATED;
    actor->generation = 0;
    atomic_init(&actor->queue_pending_bits, 0);
    actor->ops = ops;
    actor->opaque = opaque;
    actor->queue_count = queue_count;
    actor->notifier_ready = true;
    actor->thread_joined = true;
    return 0;
}

void virtio_actor_destroy(struct virtio_actor *actor)
{
    if (!actor)
        return;

    if (actor->thread_started && !actor->thread_joined) {
        pthread_mutex_lock(&actor->lock);
        bool failed = actor->state == VIRTIO_ACTOR_FAILED;
        pthread_mutex_unlock(&actor->lock);
        if (!failed)
            virtio_actor_stop(actor);
        else {
            virtio_actor_wake(actor);
            pthread_join(actor->thread, NULL);
            actor->thread_joined = true;
        }
    }

    if (actor->notifier_ready)
        semu_event_notifier_destroy(&actor->wake_notifier);
    pthread_cond_destroy(&actor->cond);
    pthread_mutex_destroy(&actor->lock);
}

int virtio_actor_start(struct virtio_actor *actor)
{
    int ret;

    if (!actor)
        return -EINVAL;

    ret = virtio_actor_lock(actor);
    if (ret < 0)
        return ret;
    if (actor->thread_started) {
        pthread_mutex_unlock(&actor->lock);
        return -EALREADY;
    }
    if (virtio_actor_terminal(actor->state)) {
        pthread_mutex_unlock(&actor->lock);
        return -EINVAL;
    }
    actor->thread_started = true;
    actor->thread_joined = false;
    pthread_mutex_unlock(&actor->lock);

    ret = pthread_create(&actor->thread, NULL, virtio_actor_thread_main, actor);
    if (ret != 0) {
        pthread_mutex_lock(&actor->lock);
        actor->thread_started = false;
        actor->thread_joined = true;
        pthread_mutex_unlock(&actor->lock);
        return -ret;
    }
    return 0;
}

int virtio_actor_enter_configuring(struct virtio_actor *actor)
{
    int ret;

    if (!actor)
        return -EINVAL;
    ret = virtio_actor_lock(actor);
    if (ret < 0)
        return ret;
    if (!virtio_actor_can_configure(actor->state)) {
        pthread_mutex_unlock(&actor->lock);
        return -EINVAL;
    }
    actor->state = VIRTIO_ACTOR_CONFIGURING;
    return virtio_actor_broadcast_unlock(actor);
}

int virtio_actor_activate(struct virtio_actor *actor)
{
    int ret;

    if (!actor)
        return -EINVAL;
    ret = virtio_actor_lock(actor);
    if (ret < 0)
        return ret;
    if (!virtio_actor_can_activate(actor->state)) {
        pthread_mutex_unlock(&actor->lock);
        return -EINVAL;
    }
    actor->state = VIRTIO_ACTOR_ACTIVE;
    ret = virtio_actor_broadcast_unlock(actor);
    if (ret < 0)
        return ret;
    return virtio_actor_wake(actor);
}

int virtio_actor_pause(struct virtio_actor *actor)
{
    int ret;

    if (!actor)
        return -EINVAL;
    ret = virtio_actor_lock(actor);
    if (ret < 0)
        return ret;
    if (actor->state == VIRTIO_ACTOR_PAUSED) {
        pthread_mutex_unlock(&actor->lock);
        return 0;
    }
    if (actor->state != VIRTIO_ACTOR_ACTIVE) {
        pthread_mutex_unlock(&actor->lock);
        return -EINVAL;
    }

    if (!actor->thread_started) {
        actor->state = VIRTIO_ACTOR_PAUSED;
        return virtio_actor_broadcast_unlock(actor);
    }

    actor->state = VIRTIO_ACTOR_PAUSING;
    pthread_cond_broadcast(&actor->cond);
    pthread_mutex_unlock(&actor->lock);
    ret = virtio_actor_wake(actor);
    if (ret < 0)
        return ret;
    return virtio_actor_wait_until(actor, VIRTIO_ACTOR_PAUSED);
}

int virtio_actor_reset(struct virtio_actor *actor)
{
    int ret;

    if (!actor)
        return -EINVAL;
    ret = virtio_actor_lock(actor);
    if (ret < 0)
        return ret;
    if (virtio_actor_terminal(actor->state)) {
        pthread_mutex_unlock(&actor->lock);
        return -EINVAL;
    }

    actor->state = VIRTIO_ACTOR_RESETTING;
    actor->generation++;
    atomic_store_explicit(&actor->queue_pending_bits, 0, memory_order_release);
    while (actor->inside_backend_mutation) {
        ret = pthread_cond_wait(&actor->cond, &actor->lock);
        if (ret != 0) {
            pthread_mutex_unlock(&actor->lock);
            return -ret;
        }
    }
    ret = virtio_actor_broadcast_unlock(actor);
    if (ret < 0)
        return ret;
    return virtio_actor_wake(actor);
}

int virtio_actor_stop(struct virtio_actor *actor)
{
    int ret;
    bool join_thread;

    if (!actor)
        return -EINVAL;
    ret = virtio_actor_lock(actor);
    if (ret < 0)
        return ret;
    if (actor->state == VIRTIO_ACTOR_STOPPED) {
        pthread_mutex_unlock(&actor->lock);
        return 0;
    }
    if (actor->state == VIRTIO_ACTOR_FAILED) {
        join_thread = actor->thread_started && !actor->thread_joined;
        pthread_mutex_unlock(&actor->lock);
        if (join_thread) {
            virtio_actor_wake(actor);
            pthread_join(actor->thread, NULL);
            actor->thread_joined = true;
        }
        return 0;
    }

    actor->state = VIRTIO_ACTOR_STOPPING;
    actor->generation++;
    actor->stop_requested = true;
    actor->wake_pending = true;
    atomic_store_explicit(&actor->queue_pending_bits, 0, memory_order_release);
    join_thread = actor->thread_started && !actor->thread_joined;
    if (!join_thread)
        actor->state = VIRTIO_ACTOR_STOPPED;
    ret = virtio_actor_broadcast_unlock(actor);
    if (ret < 0)
        return ret;
    ret = virtio_actor_wake(actor);
    if (ret < 0)
        return ret;
    if (join_thread) {
        ret = pthread_join(actor->thread, NULL);
        if (ret != 0)
            return -ret;
        actor->thread_joined = true;
    }
    return 0;
}

int virtio_actor_fail(struct virtio_actor *actor)
{
    int ret;

    if (!actor)
        return -EINVAL;
    ret = virtio_actor_lock(actor);
    if (ret < 0)
        return ret;
    if (actor->state == VIRTIO_ACTOR_FAILED) {
        pthread_mutex_unlock(&actor->lock);
        return 0;
    }

    actor->state = VIRTIO_ACTOR_FAILED;
    actor->generation++;
    actor->failed = true;
    actor->wake_pending = true;
    atomic_store_explicit(&actor->queue_pending_bits, 0, memory_order_release);
    ret = virtio_actor_broadcast_unlock(actor);
    if (ret < 0)
        return ret;
    return virtio_actor_wake(actor);
}

int virtio_actor_wake(struct virtio_actor *actor)
{
    int ret;

    if (!actor)
        return -EINVAL;
    ret = virtio_actor_lock(actor);
    if (ret < 0)
        return ret;
    actor->wake_pending = true;
    ret = virtio_actor_broadcast_unlock(actor);
    if (ret < 0)
        return ret;
    if (actor->notifier_ready)
        return semu_event_notifier_signal(&actor->wake_notifier);
    return 0;
}

int virtio_actor_ack_wake(struct virtio_actor *actor)
{
    int ret;

    if (!actor)
        return -EINVAL;
    ret = virtio_actor_lock(actor);
    if (ret < 0)
        return ret;
    actor->wake_pending = false;
    ret = virtio_actor_unlock(actor);
    if (ret < 0)
        return ret;
    if (actor->notifier_ready)
        return semu_event_notifier_drain(&actor->wake_notifier);
    return 0;
}

int virtio_actor_notify_queue(struct virtio_actor *actor, uint16_t queue_index)
{
    unsigned mask;
    unsigned old;
    bool signal = false;
    bool notifier_ready;
    int ret;

    if (!virtio_actor_queue_valid(actor, queue_index))
        return -EINVAL;

    mask = 1u << queue_index;
    ret = virtio_actor_lock(actor);
    if (ret < 0)
        return ret;
    if (actor->state != VIRTIO_ACTOR_ACTIVE || actor->failed ||
        actor->stop_requested) {
        pthread_mutex_unlock(&actor->lock);
        return actor->failed ? -EIO : -EAGAIN;
    }

    old = atomic_fetch_or_explicit(&actor->queue_pending_bits, mask,
                                   memory_order_acq_rel);
    if ((old & mask) == 0) {
        actor->wake_pending = true;
        signal = true;
        ret = pthread_cond_broadcast(&actor->cond);
        if (ret != 0) {
            pthread_mutex_unlock(&actor->lock);
            return -ret;
        }
    }
    notifier_ready = actor->notifier_ready;
    ret = virtio_actor_unlock(actor);
    if (ret < 0)
        return ret;
    if (signal && notifier_ready)
        return semu_event_notifier_signal(&actor->wake_notifier);
    return 0;
}

bool virtio_actor_queue_valid(const struct virtio_actor *actor,
                              uint16_t queue_index)
{
    return actor && queue_index < actor->queue_count &&
           queue_index < sizeof(unsigned) * CHAR_BIT;
}

uint32_t virtio_actor_pending_mask(const struct virtio_actor *actor)
{
    if (!actor)
        return 0;
    return atomic_load_explicit(&actor->queue_pending_bits,
                                memory_order_acquire);
}

void virtio_actor_clear_queue_pending(struct virtio_actor *actor,
                                      uint16_t queue_index)
{
    if (!virtio_actor_queue_valid(actor, queue_index))
        return;
    unsigned mask = 1u << queue_index;
    atomic_fetch_and_explicit(&actor->queue_pending_bits, ~mask,
                              memory_order_release);
}

enum virtio_actor_state virtio_actor_get_state(const struct virtio_actor *actor)
{
    struct virtio_actor *mutable_actor;
    enum virtio_actor_state state;

    if (!actor)
        return VIRTIO_ACTOR_FAILED;

    mutable_actor = virtio_actor_mutable(actor);
    pthread_mutex_lock(&mutable_actor->lock);
    state = actor->state;
    pthread_mutex_unlock(&mutable_actor->lock);
    return state;
}

uint64_t virtio_actor_generation(const struct virtio_actor *actor)
{
    struct virtio_actor *mutable_actor;
    uint64_t generation;

    if (!actor)
        return 0;

    mutable_actor = virtio_actor_mutable(actor);
    pthread_mutex_lock(&mutable_actor->lock);
    generation = actor->generation;
    pthread_mutex_unlock(&mutable_actor->lock);
    return generation;
}

int virtio_actor_wait_changed_from(struct virtio_actor *actor,
                                   enum virtio_actor_state state,
                                   enum virtio_actor_state *observed)
{
    int ret;

    if (!actor || !observed || !virtio_actor_state_valid(state))
        return -EINVAL;

    ret = virtio_actor_lock(actor);
    if (ret < 0)
        return ret;
    while (actor->state == state) {
        ret = pthread_cond_wait(&actor->cond, &actor->lock);
        if (ret != 0) {
            pthread_mutex_unlock(&actor->lock);
            return -ret;
        }
    }
    *observed = actor->state;
    return virtio_actor_unlock(actor);
}

int virtio_actor_wait_until(struct virtio_actor *actor,
                            enum virtio_actor_state target)
{
    int ret;

    if (!actor || !virtio_actor_state_valid(target))
        return -EINVAL;

    ret = virtio_actor_lock(actor);
    if (ret < 0)
        return ret;
    while (actor->state != target) {
        ret = virtio_actor_wait_terminal_error(actor->state, target);
        if (ret < 0) {
            pthread_mutex_unlock(&actor->lock);
            return ret;
        }

        ret = pthread_cond_wait(&actor->cond, &actor->lock);
        if (ret != 0) {
            pthread_mutex_unlock(&actor->lock);
            return -ret;
        }
    }
    return virtio_actor_unlock(actor);
}

bool virtio_actor_begin_completion(struct virtio_actor *actor,
                                   uint64_t generation)
{
    if (!actor)
        return false;

    if (virtio_actor_lock(actor) < 0)
        return false;
    if (actor->state == VIRTIO_ACTOR_ACTIVE && actor->generation == generation)
        return true;

    pthread_mutex_unlock(&actor->lock);
    return false;
}

int virtio_actor_end_completion(struct virtio_actor *actor)
{
    if (!actor)
        return -EINVAL;
    return virtio_actor_unlock(actor);
}

bool virtio_actor_lock_is_held(struct virtio_actor *actor)
{
    int ret;

    if (!actor)
        return false;

    ret = pthread_mutex_trylock(&actor->lock);
    if (ret == 0) {
        pthread_mutex_unlock(&actor->lock);
        return false;
    }
    return ret == EBUSY;
}
