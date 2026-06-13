#include "lock-order.h"

#include <errno.h>

#define SEMU_LOCK_ORDER_MAX_DEPTH 32

static _Thread_local enum semu_lock_rank
    semu_lock_order_stack[SEMU_LOCK_ORDER_MAX_DEPTH];
static _Thread_local unsigned int semu_lock_order_depth;

static bool semu_lock_rank_valid(enum semu_lock_rank rank)
{
    return rank == SEMU_LOCK_RANK_VM_LIFECYCLE ||
           rank == SEMU_LOCK_RANK_DEVICE_TRANSPORT ||
           rank == SEMU_LOCK_RANK_ACTOR_MAILBOX ||
           rank == SEMU_LOCK_RANK_QUEUE_STATE ||
           rank == SEMU_LOCK_RANK_BACKEND_LOCAL;
}

static int semu_lock_order_validate_leave(
    const struct semu_lock_order_guard *guard)
{
    if (!guard || !guard->held)
        return -EINVAL;
    if (semu_lock_order_depth == 0)
        return -EPERM;
    if (guard->depth + 1 != semu_lock_order_depth)
        return -EPERM;
    if (semu_lock_order_stack[semu_lock_order_depth - 1] != guard->rank)
        return -EPERM;
    return 0;
}

static int semu_lock_order_pop(struct semu_lock_order_guard *guard)
{
    semu_lock_order_depth--;
    semu_lock_order_stack[semu_lock_order_depth] = SEMU_LOCK_RANK_NONE;
    guard->rank = SEMU_LOCK_RANK_NONE;
    guard->depth = 0;
    guard->held = false;
    return 0;
}

enum semu_lock_rank semu_lock_order_current_rank(void)
{
    if (semu_lock_order_depth == 0)
        return SEMU_LOCK_RANK_NONE;
    return semu_lock_order_stack[semu_lock_order_depth - 1];
}

unsigned int semu_lock_order_current_depth(void)
{
    return semu_lock_order_depth;
}

int semu_lock_order_enter(enum semu_lock_rank rank,
                          struct semu_lock_order_guard *guard)
{
    enum semu_lock_rank current;

    if (!guard || !semu_lock_rank_valid(rank))
        return -EINVAL;

    guard->rank = SEMU_LOCK_RANK_NONE;
    guard->depth = 0;
    guard->held = false;

    current = semu_lock_order_current_rank();
    if (current != SEMU_LOCK_RANK_NONE && rank < current)
        return -EDEADLK;
    if (semu_lock_order_depth >= SEMU_LOCK_ORDER_MAX_DEPTH)
        return -EOVERFLOW;

    guard->rank = rank;
    guard->depth = semu_lock_order_depth;
    guard->held = true;
    semu_lock_order_stack[semu_lock_order_depth++] = rank;
    return 0;
}

int semu_lock_order_leave(struct semu_lock_order_guard *guard)
{
    int ret = semu_lock_order_validate_leave(guard);

    if (ret < 0)
        return ret;
    return semu_lock_order_pop(guard);
}

int semu_lock_order_mutex_lock(pthread_mutex_t *mutex,
                               enum semu_lock_rank rank,
                               struct semu_lock_order_guard *guard)
{
    int ret;

    if (!mutex)
        return -EINVAL;
    ret = semu_lock_order_enter(rank, guard);
    if (ret < 0)
        return ret;

    ret = pthread_mutex_lock(mutex);
    if (ret != 0) {
        (void) semu_lock_order_pop(guard);
        return -ret;
    }
    return 0;
}

int semu_lock_order_mutex_trylock(pthread_mutex_t *mutex,
                                  enum semu_lock_rank rank,
                                  struct semu_lock_order_guard *guard)
{
    int ret;

    if (!mutex)
        return -EINVAL;
    ret = semu_lock_order_enter(rank, guard);
    if (ret < 0)
        return ret;

    ret = pthread_mutex_trylock(mutex);
    if (ret != 0) {
        (void) semu_lock_order_pop(guard);
        return -ret;
    }
    return 0;
}

int semu_lock_order_mutex_unlock(pthread_mutex_t *mutex,
                                 struct semu_lock_order_guard *guard)
{
    int ret;

    if (!mutex)
        return -EINVAL;
    ret = semu_lock_order_validate_leave(guard);
    if (ret < 0)
        return ret;

    ret = pthread_mutex_unlock(mutex);
    if (ret != 0)
        return -ret;
    return semu_lock_order_pop(guard);
}
