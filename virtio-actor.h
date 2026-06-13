#ifndef SEMU_VIRTIO_ACTOR_H
#define SEMU_VIRTIO_ACTOR_H

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "semu-event.h"

struct virtio_actor;

enum virtio_actor_state {
    VIRTIO_ACTOR_CREATED,
    VIRTIO_ACTOR_CONFIGURING,
    VIRTIO_ACTOR_ACTIVE,
    VIRTIO_ACTOR_PAUSING,
    VIRTIO_ACTOR_PAUSED,
    VIRTIO_ACTOR_DRAINING,
    VIRTIO_ACTOR_RESETTING,
    VIRTIO_ACTOR_STOPPING,
    VIRTIO_ACTOR_STOPPED,
    VIRTIO_ACTOR_FAILED,
};

struct virtio_actor_ops {
    int (*drain_queue)(void *opaque,
                       struct virtio_actor *actor,
                       uint16_t queue_index,
                       uint64_t generation);
    bool (*queue_has_work)(void *opaque,
                           struct virtio_actor *actor,
                           uint16_t queue_index,
                           uint64_t generation);
    void (*on_failed)(void *opaque, struct virtio_actor *actor);
};

struct virtio_actor {
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    enum virtio_actor_state state;
    uint64_t generation;
    atomic_uint queue_pending_bits;
    bool wake_pending;
    bool stop_requested;
    bool failed;

    const struct virtio_actor_ops *ops;
    void *opaque;
    uint16_t queue_count;
    bool notifier_ready;
    bool thread_started;
    bool thread_joined;
    bool inside_backend_mutation;
    struct semu_event_notifier wake_notifier;
};

int virtio_actor_init(struct virtio_actor *actor,
                      const struct virtio_actor_ops *ops,
                      void *opaque,
                      uint16_t queue_count);
void virtio_actor_destroy(struct virtio_actor *actor);

int virtio_actor_start(struct virtio_actor *actor);
int virtio_actor_enter_configuring(struct virtio_actor *actor);
int virtio_actor_activate(struct virtio_actor *actor);
int virtio_actor_pause(struct virtio_actor *actor);
int virtio_actor_reset(struct virtio_actor *actor);
int virtio_actor_stop(struct virtio_actor *actor);
int virtio_actor_fail(struct virtio_actor *actor);

int virtio_actor_wake(struct virtio_actor *actor);
int virtio_actor_ack_wake(struct virtio_actor *actor);

bool virtio_actor_queue_valid(const struct virtio_actor *actor,
                              uint16_t queue_index);
uint32_t virtio_actor_pending_mask(const struct virtio_actor *actor);
void virtio_actor_clear_queue_pending(struct virtio_actor *actor,
                                      uint16_t queue_index);

int virtio_actor_notify_queue(struct virtio_actor *actor, uint16_t queue_index);

enum virtio_actor_state virtio_actor_get_state(
    const struct virtio_actor *actor);
uint64_t virtio_actor_generation(const struct virtio_actor *actor);

int virtio_actor_wait_changed_from(struct virtio_actor *actor,
                                   enum virtio_actor_state state,
                                   enum virtio_actor_state *observed);
int virtio_actor_wait_until(struct virtio_actor *actor,
                            enum virtio_actor_state target);
bool virtio_actor_begin_completion(struct virtio_actor *actor,
                                   uint64_t generation);
int virtio_actor_end_completion(struct virtio_actor *actor);
bool virtio_actor_lock_is_held(struct virtio_actor *actor);

#endif
