#include "vm-lifecycle.h"

#include <errno.h>
#include <string.h>

static bool semu_vm_lifecycle_state_valid(enum semu_vm_lifecycle_state state)
{
    return state >= SEMU_VM_CREATED && state <= SEMU_VM_FAILED;
}

static struct semu_vm_lifecycle *semu_vm_lifecycle_mutable(
    const struct semu_vm_lifecycle *lc)
{
    return (struct semu_vm_lifecycle *) lc;
}

static enum semu_vm_lifecycle_state semu_vm_lifecycle_state_locked(
    const struct semu_vm_lifecycle *lc)
{
    return lc->state;
}

static bool semu_vm_lifecycle_can_enter_running(
    enum semu_vm_lifecycle_state state)
{
    return state == SEMU_VM_CREATED || state == SEMU_VM_PAUSED ||
           state == SEMU_VM_RESETTING;
}

static bool semu_vm_lifecycle_can_enter_draining(
    enum semu_vm_lifecycle_state state)
{
    return state == SEMU_VM_RUNNING || state == SEMU_VM_PAUSE_REQUESTED ||
           state == SEMU_VM_PAUSED;
}

static bool semu_vm_lifecycle_can_enter_resetting(
    enum semu_vm_lifecycle_state state)
{
    return state == SEMU_VM_RUNNING || state == SEMU_VM_PAUSE_REQUESTED ||
           state == SEMU_VM_PAUSED || state == SEMU_VM_DRAINING;
}

static bool semu_vm_lifecycle_can_enter_stopping(
    enum semu_vm_lifecycle_state state)
{
    return state != SEMU_VM_STOPPING && state != SEMU_VM_STOPPED &&
           state != SEMU_VM_FAILED;
}

static bool semu_vm_lifecycle_can_enter_stopped(
    enum semu_vm_lifecycle_state state)
{
    return state == SEMU_VM_STOPPING || state == SEMU_VM_CREATED ||
           state == SEMU_VM_RUNNING || state == SEMU_VM_PAUSE_REQUESTED ||
           state == SEMU_VM_PAUSED || state == SEMU_VM_DRAINING ||
           state == SEMU_VM_RESETTING;
}

static bool semu_vm_lifecycle_can_enter_failed(
    enum semu_vm_lifecycle_state state)
{
    return state != SEMU_VM_FAILED;
}

static int semu_vm_lifecycle_broadcast_unlock(struct semu_vm_lifecycle *lc)
{
    int ret = pthread_cond_broadcast(&lc->cond);
    int unlock_ret = pthread_mutex_unlock(&lc->lock);

    if (ret != 0)
        return -ret;
    if (unlock_ret != 0)
        return -unlock_ret;
    return 0;
}

int semu_vm_lifecycle_init(struct semu_vm_lifecycle *lc)
{
    int ret;

    if (!lc)
        return -EINVAL;

    memset(lc, 0, sizeof(*lc));
    ret = pthread_mutex_init(&lc->lock, NULL);
    if (ret != 0)
        return -ret;
    ret = pthread_cond_init(&lc->cond, NULL);
    if (ret != 0) {
        pthread_mutex_destroy(&lc->lock);
        return -ret;
    }

    lc->state = SEMU_VM_CREATED;
    lc->generation = 0;
    lc->pause_seq = 0;
    lc->drain_seq = 0;
    lc->accepting_device_work = false;
    return 0;
}

void semu_vm_lifecycle_destroy(struct semu_vm_lifecycle *lc)
{
    if (!lc)
        return;

    pthread_cond_destroy(&lc->cond);
    pthread_mutex_destroy(&lc->lock);
}

enum semu_vm_lifecycle_state semu_vm_lifecycle_state(
    const struct semu_vm_lifecycle *lc)
{
    struct semu_vm_lifecycle *mutable_lc;
    enum semu_vm_lifecycle_state state;

    if (!lc)
        return SEMU_VM_FAILED;

    mutable_lc = semu_vm_lifecycle_mutable(lc);
    pthread_mutex_lock(&mutable_lc->lock);
    state = semu_vm_lifecycle_state_locked(lc);
    pthread_mutex_unlock(&mutable_lc->lock);
    return state;
}

uint64_t semu_vm_lifecycle_generation(const struct semu_vm_lifecycle *lc)
{
    struct semu_vm_lifecycle *mutable_lc;
    uint64_t generation;

    if (!lc)
        return 0;

    mutable_lc = semu_vm_lifecycle_mutable(lc);
    pthread_mutex_lock(&mutable_lc->lock);
    generation = lc->generation;
    pthread_mutex_unlock(&mutable_lc->lock);
    return generation;
}

uint64_t semu_vm_lifecycle_pause_seq(const struct semu_vm_lifecycle *lc)
{
    struct semu_vm_lifecycle *mutable_lc;
    uint64_t pause_seq;

    if (!lc)
        return 0;

    mutable_lc = semu_vm_lifecycle_mutable(lc);
    pthread_mutex_lock(&mutable_lc->lock);
    pause_seq = lc->pause_seq;
    pthread_mutex_unlock(&mutable_lc->lock);
    return pause_seq;
}

uint64_t semu_vm_lifecycle_drain_seq(const struct semu_vm_lifecycle *lc)
{
    struct semu_vm_lifecycle *mutable_lc;
    uint64_t drain_seq;

    if (!lc)
        return 0;

    mutable_lc = semu_vm_lifecycle_mutable(lc);
    pthread_mutex_lock(&mutable_lc->lock);
    drain_seq = lc->drain_seq;
    pthread_mutex_unlock(&mutable_lc->lock);
    return drain_seq;
}

bool semu_vm_accepting_device_work(const struct semu_vm_lifecycle *lc)
{
    struct semu_vm_lifecycle *mutable_lc;
    bool accepting;

    if (!lc)
        return false;

    mutable_lc = semu_vm_lifecycle_mutable(lc);
    pthread_mutex_lock(&mutable_lc->lock);
    accepting = lc->accepting_device_work;
    pthread_mutex_unlock(&mutable_lc->lock);
    return accepting;
}

int semu_vm_lifecycle_enter_running(struct semu_vm_lifecycle *lc)
{
    int ret;

    if (!lc)
        return -EINVAL;

    ret = pthread_mutex_lock(&lc->lock);
    if (ret != 0)
        return -ret;
    if (!semu_vm_lifecycle_can_enter_running(lc->state)) {
        pthread_mutex_unlock(&lc->lock);
        return -EINVAL;
    }

    lc->state = SEMU_VM_RUNNING;
    lc->accepting_device_work = true;
    return semu_vm_lifecycle_broadcast_unlock(lc);
}

int semu_vm_lifecycle_request_pause(struct semu_vm_lifecycle *lc)
{
    int ret;

    if (!lc)
        return -EINVAL;

    ret = pthread_mutex_lock(&lc->lock);
    if (ret != 0)
        return -ret;
    if (lc->state != SEMU_VM_RUNNING) {
        pthread_mutex_unlock(&lc->lock);
        return -EINVAL;
    }

    lc->state = SEMU_VM_PAUSE_REQUESTED;
    lc->pause_seq++;
    lc->accepting_device_work = false;
    return semu_vm_lifecycle_broadcast_unlock(lc);
}

int semu_vm_lifecycle_enter_paused(struct semu_vm_lifecycle *lc)
{
    int ret;

    if (!lc)
        return -EINVAL;

    ret = pthread_mutex_lock(&lc->lock);
    if (ret != 0)
        return -ret;
    if (lc->state != SEMU_VM_PAUSE_REQUESTED) {
        pthread_mutex_unlock(&lc->lock);
        return -EINVAL;
    }

    lc->state = SEMU_VM_PAUSED;
    lc->accepting_device_work = false;
    return semu_vm_lifecycle_broadcast_unlock(lc);
}

int semu_vm_lifecycle_enter_draining(struct semu_vm_lifecycle *lc)
{
    int ret;

    if (!lc)
        return -EINVAL;

    ret = pthread_mutex_lock(&lc->lock);
    if (ret != 0)
        return -ret;
    if (!semu_vm_lifecycle_can_enter_draining(lc->state)) {
        pthread_mutex_unlock(&lc->lock);
        return -EINVAL;
    }

    lc->state = SEMU_VM_DRAINING;
    lc->drain_seq++;
    lc->accepting_device_work = false;
    return semu_vm_lifecycle_broadcast_unlock(lc);
}

int semu_vm_lifecycle_enter_resetting(struct semu_vm_lifecycle *lc)
{
    int ret;

    if (!lc)
        return -EINVAL;

    ret = pthread_mutex_lock(&lc->lock);
    if (ret != 0)
        return -ret;
    if (!semu_vm_lifecycle_can_enter_resetting(lc->state)) {
        pthread_mutex_unlock(&lc->lock);
        return -EINVAL;
    }

    lc->state = SEMU_VM_RESETTING;
    lc->generation++;
    lc->accepting_device_work = false;
    return semu_vm_lifecycle_broadcast_unlock(lc);
}

int semu_vm_lifecycle_enter_stopping(struct semu_vm_lifecycle *lc)
{
    int ret;

    if (!lc)
        return -EINVAL;

    ret = pthread_mutex_lock(&lc->lock);
    if (ret != 0)
        return -ret;
    if (!semu_vm_lifecycle_can_enter_stopping(lc->state)) {
        pthread_mutex_unlock(&lc->lock);
        return -EINVAL;
    }

    lc->state = SEMU_VM_STOPPING;
    lc->generation++;
    lc->accepting_device_work = false;
    return semu_vm_lifecycle_broadcast_unlock(lc);
}

int semu_vm_lifecycle_enter_stopped(struct semu_vm_lifecycle *lc)
{
    int ret;

    if (!lc)
        return -EINVAL;

    ret = pthread_mutex_lock(&lc->lock);
    if (ret != 0)
        return -ret;
    if (!semu_vm_lifecycle_can_enter_stopped(lc->state)) {
        pthread_mutex_unlock(&lc->lock);
        return -EINVAL;
    }

    if (lc->state != SEMU_VM_STOPPING)
        lc->generation++;
    lc->state = SEMU_VM_STOPPED;
    lc->accepting_device_work = false;
    return semu_vm_lifecycle_broadcast_unlock(lc);
}

int semu_vm_lifecycle_enter_failed(struct semu_vm_lifecycle *lc)
{
    int ret;

    if (!lc)
        return -EINVAL;

    ret = pthread_mutex_lock(&lc->lock);
    if (ret != 0)
        return -ret;
    if (!semu_vm_lifecycle_can_enter_failed(lc->state)) {
        pthread_mutex_unlock(&lc->lock);
        return -EINVAL;
    }

    lc->state = SEMU_VM_FAILED;
    lc->accepting_device_work = false;
    return semu_vm_lifecycle_broadcast_unlock(lc);
}

int semu_vm_lifecycle_wait_changed_from(struct semu_vm_lifecycle *lc,
                                        enum semu_vm_lifecycle_state state,
                                        enum semu_vm_lifecycle_state *observed)
{
    int ret;

    if (!lc || !observed || !semu_vm_lifecycle_state_valid(state))
        return -EINVAL;

    ret = pthread_mutex_lock(&lc->lock);
    if (ret != 0)
        return -ret;
    while (lc->state == state) {
        ret = pthread_cond_wait(&lc->cond, &lc->lock);
        if (ret != 0) {
            pthread_mutex_unlock(&lc->lock);
            return -ret;
        }
    }

    *observed = lc->state;
    ret = pthread_mutex_unlock(&lc->lock);
    if (ret != 0)
        return -ret;
    return 0;
}

int semu_vm_lifecycle_wait_until(struct semu_vm_lifecycle *lc,
                                 enum semu_vm_lifecycle_state target)
{
    int ret;

    if (!lc || !semu_vm_lifecycle_state_valid(target))
        return -EINVAL;

    ret = pthread_mutex_lock(&lc->lock);
    if (ret != 0)
        return -ret;
    while (lc->state != target) {
        ret = pthread_cond_wait(&lc->cond, &lc->lock);
        if (ret != 0) {
            pthread_mutex_unlock(&lc->lock);
            return -ret;
        }
    }

    ret = pthread_mutex_unlock(&lc->lock);
    if (ret != 0)
        return -ret;
    return 0;
}
