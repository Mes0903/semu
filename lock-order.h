#ifndef SEMU_LOCK_ORDER_H
#define SEMU_LOCK_ORDER_H

#include <pthread.h>
#include <stdbool.h>

/* Lock ranks may skip values, but a thread must never acquire a lower
 * rank while a higher rank is tracked on that same thread.
 */
enum semu_lock_rank {
    SEMU_LOCK_RANK_NONE = 0,
    SEMU_LOCK_RANK_VM_LIFECYCLE = 10,
    SEMU_LOCK_RANK_DEVICE_TRANSPORT = 20,
    SEMU_LOCK_RANK_ACTOR_MAILBOX = 30,
    SEMU_LOCK_RANK_QUEUE_STATE = 40,
    SEMU_LOCK_RANK_BACKEND_LOCAL = 50,
};

struct semu_lock_order_guard {
    enum semu_lock_rank rank;
    unsigned int depth;
    bool held;
};

enum semu_lock_rank semu_lock_order_current_rank(void);
unsigned int semu_lock_order_current_depth(void);
int semu_lock_order_enter(enum semu_lock_rank rank,
                          struct semu_lock_order_guard *guard);
int semu_lock_order_leave(struct semu_lock_order_guard *guard);
int semu_lock_order_mutex_lock(pthread_mutex_t *mutex,
                               enum semu_lock_rank rank,
                               struct semu_lock_order_guard *guard);
int semu_lock_order_mutex_trylock(pthread_mutex_t *mutex,
                                  enum semu_lock_rank rank,
                                  struct semu_lock_order_guard *guard);
int semu_lock_order_mutex_unlock(pthread_mutex_t *mutex,
                                 struct semu_lock_order_guard *guard);

#endif
