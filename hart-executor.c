#include "device.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

struct hart_executor_dedicated_state {
    pthread_t *hart_threads;
    pthread_t io_thread;
    bool io_thread_created;
    uint32_t created_harts;
    uint32_t initialized_hart_waits;
    _Atomic bool fatal;
    hart_wait_t *hart_wait;
};

static inline bool emu_stopped_load_executor(const emu_state_t *emu)
{
    return __atomic_load_n(&emu->stopped, __ATOMIC_RELAXED);
}

static inline void emu_stopped_store_executor(emu_state_t *emu, bool value)
{
    __atomic_store_n(&emu->stopped, value, __ATOMIC_RELAXED);
}

static struct hart_executor_dedicated_state *dedicated_state(emu_state_t *emu)
{
    if (!emu || emu->executor.backend != HART_EXEC_DEDICATED_THREADS)
        return NULL;
    return emu->executor.backend_state;
}

static int init_hart_waits(struct hart_executor_dedicated_state *state,
                           uint32_t n_hart)
{
    state->hart_wait = calloc(n_hart, sizeof(*state->hart_wait));
    state->hart_threads = calloc(n_hart, sizeof(*state->hart_threads));
    if (!state->hart_wait || !state->hart_threads)
        return -ENOMEM;

    for (uint32_t i = 0; i < n_hart; i++) {
        int rc = pthread_mutex_init(&state->hart_wait[i].mutex, NULL);
        if (rc != 0)
            return -rc;
        rc = pthread_cond_init(&state->hart_wait[i].cond, NULL);
        if (rc != 0) {
            pthread_mutex_destroy(&state->hart_wait[i].mutex);
            return -rc;
        }
        state->initialized_hart_waits++;
    }
    return 0;
}

static void destroy_hart_waits(struct hart_executor_dedicated_state *state,
                               uint32_t n_hart)
{
    if (!state)
        return;

    if (state->hart_wait) {
        uint32_t initialized = MIN(state->initialized_hart_waits, n_hart);
        for (uint32_t i = 0; i < initialized; i++) {
            pthread_cond_destroy(&state->hart_wait[i].cond);
            pthread_mutex_destroy(&state->hart_wait[i].mutex);
        }
        free(state->hart_wait);
    }
    free(state->hart_threads);
}

static int dedicated_start(struct emu_state *emu_opaque)
{
    emu_state_t *emu = emu_opaque;
    struct hart_executor_dedicated_state *state = dedicated_state(emu);
    if (!state || !emu->executor.hart_thread_fn || !emu->executor.io_thread_fn)
        return -EINVAL;

    state->created_harts = 0;
    int rc = pthread_create(&state->io_thread, NULL, emu->executor.io_thread_fn,
                            emu);
    if (rc != 0) {
        hart_executor_mark_fatal(emu);
        return -rc;
    }
    state->io_thread_created = true;

    for (uint32_t i = 0; i < emu->vm.n_hart; i++) {
        rc = pthread_create(&state->hart_threads[i], NULL,
                            emu->executor.hart_thread_fn, emu->vm.hart[i]);
        if (rc != 0) {
            hart_executor_mark_fatal(emu);
            hart_executor_request_stop(emu);
            return -rc;
        }
        state->created_harts++;
    }

    return 0;
}

static void dedicated_request_stop(struct emu_state *emu_opaque)
{
    emu_state_t *emu = emu_opaque;
    emu_stopped_store_executor(emu, true);
    hart_executor_wake_all(emu);
}

static void dedicated_wake_hart(struct emu_state *emu_opaque, uint32_t hart_id)
{
    emu_state_t *emu = emu_opaque;
    hart_wait_t *wait = hart_executor_wait_for_hart(emu, hart_id);
    if (!wait)
        return;

    pthread_mutex_lock(&wait->mutex);
    pthread_cond_signal(&wait->cond);
    pthread_mutex_unlock(&wait->mutex);
}

static int dedicated_join(struct emu_state *emu_opaque)
{
    emu_state_t *emu = emu_opaque;
    struct hart_executor_dedicated_state *state = dedicated_state(emu);
    if (!state)
        return -EINVAL;

    for (uint32_t i = 0; i < state->created_harts; i++)
        pthread_join(state->hart_threads[i], NULL);
    state->created_harts = 0;

    if (state->io_thread_created) {
        pthread_join(state->io_thread, NULL);
        state->io_thread_created = false;
    }

    return 0;
}

static int single_start(struct emu_state *emu_opaque)
{
    emu_state_t *emu = emu_opaque;
    if (emu->executor.single_thread_fn)
        emu->executor.single_thread_fn(emu);
    return 0;
}

static void single_request_stop(struct emu_state *emu_opaque)
{
    emu_state_t *emu = emu_opaque;
    emu_stopped_store_executor(emu, true);
}

static int noop_join(struct emu_state *emu_opaque UNUSED)
{
    return 0;
}

static const struct hart_executor_ops dedicated_ops = {
    .start = dedicated_start,
    .request_stop = dedicated_request_stop,
    .wake_hart = dedicated_wake_hart,
    .join = dedicated_join,
};

static const struct hart_executor_ops single_ops = {
    .start = single_start,
    .request_stop = single_request_stop,
    .wake_hart = NULL,
    .join = noop_join,
};

int hart_executor_init(struct emu_state *emu_opaque,
                       enum hart_executor_backend backend,
                       hart_executor_hart_thread_fn hart_thread_fn,
                       hart_executor_io_thread_fn io_thread_fn,
                       hart_executor_single_thread_fn single_thread_fn)
{
    emu_state_t *emu = emu_opaque;
    if (!emu)
        return -EINVAL;

    emu->executor = (struct hart_executor) {
        .backend = backend,
        .hart_thread_fn = hart_thread_fn,
        .io_thread_fn = io_thread_fn,
        .single_thread_fn = single_thread_fn,
    };

    switch (backend) {
    case HART_EXEC_SINGLE_THREAD:
        emu->executor.ops = &single_ops;
        return 0;
    case HART_EXEC_DEDICATED_THREADS: {
        struct hart_executor_dedicated_state *state = calloc(1, sizeof(*state));
        if (!state)
            return -ENOMEM;
        int ret = init_hart_waits(state, emu->vm.n_hart);
        if (ret < 0) {
            destroy_hart_waits(state, emu->vm.n_hart);
            free(state);
            emu->executor = (struct hart_executor) {0};
            return ret;
        }
        emu->executor.backend_state = state;
        emu->executor.ops = &dedicated_ops;
        return 0;
    }
    case HART_EXEC_WORKER_POOL:
        return -ENOTSUP;
    default:
        return -EINVAL;
    }
}

void hart_executor_destroy(struct emu_state *emu_opaque)
{
    emu_state_t *emu = emu_opaque;
    if (!emu)
        return;

    if (emu->executor.backend == HART_EXEC_DEDICATED_THREADS) {
        struct hart_executor_dedicated_state *state = dedicated_state(emu);
        if (state) {
            hart_executor_request_stop(emu);
            hart_executor_join(emu);
            destroy_hart_waits(state, emu->vm.n_hart);
            free(state);
        }
    }

    emu->executor = (struct hart_executor) {0};
}

int hart_executor_start(struct emu_state *emu)
{
    if (!emu || !emu->executor.ops || !emu->executor.ops->start)
        return -EINVAL;
    return emu->executor.ops->start(emu);
}

void hart_executor_request_stop(struct emu_state *emu)
{
    if (!emu || !emu->executor.ops || !emu->executor.ops->request_stop) {
        if (emu)
            emu_stopped_store_executor((emu_state_t *) emu, true);
        return;
    }
    emu->executor.ops->request_stop(emu);
}

void hart_executor_wake_hart(struct emu_state *emu, uint32_t hart_id)
{
    if (!emu || !emu->executor.ops || !emu->executor.ops->wake_hart)
        return;
    emu->executor.ops->wake_hart(emu, hart_id);
}

void hart_executor_wake_all(struct emu_state *emu_opaque)
{
    emu_state_t *emu = emu_opaque;
    if (!emu)
        return;
    for (uint32_t i = 0; i < emu->vm.n_hart; i++)
        hart_executor_wake_hart(emu_opaque, i);
}

int hart_executor_request_pause(struct emu_state *emu UNUSED,
                                uint64_t pause_seq UNUSED)
{
    return -ENOTSUP;
}

int hart_executor_request_rfence(struct emu_state *emu UNUSED,
                                 uint32_t hart_mask UNUSED,
                                 uint32_t hart_mask_base UNUSED,
                                 uint64_t rfence_seq UNUSED)
{
    return -ENOTSUP;
}

int hart_executor_join(struct emu_state *emu)
{
    if (!emu || !emu->executor.ops || !emu->executor.ops->join)
        return -EINVAL;
    return emu->executor.ops->join(emu);
}

hart_wait_t *hart_executor_wait_for_hart(struct emu_state *emu_opaque,
                                         uint32_t hart_id)
{
    emu_state_t *emu = emu_opaque;
    struct hart_executor_dedicated_state *state = dedicated_state(emu);
    if (!state || hart_id >= emu->vm.n_hart)
        return NULL;
    return &state->hart_wait[hart_id];
}

void hart_executor_mark_fatal(struct emu_state *emu_opaque)
{
    emu_state_t *emu = emu_opaque;
    struct hart_executor_dedicated_state *state = dedicated_state(emu);
    if (state)
        __atomic_store_n(&state->fatal, true, __ATOMIC_RELAXED);
}

bool hart_executor_fatal(struct emu_state *emu_opaque)
{
    emu_state_t *emu = emu_opaque;
    struct hart_executor_dedicated_state *state = dedicated_state(emu);
    if (!state)
        return false;
    return __atomic_load_n(&state->fatal, __ATOMIC_RELAXED);
}
