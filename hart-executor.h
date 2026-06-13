#pragma once

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

struct emu_state;

enum hart_executor_backend {
    HART_EXEC_SINGLE_THREAD,
    HART_EXEC_DEDICATED_THREADS,
    HART_EXEC_WORKER_POOL,
};

typedef void *(*hart_executor_hart_thread_fn)(void *arg);
typedef void *(*hart_executor_io_thread_fn)(void *arg);
typedef void (*hart_executor_single_thread_fn)(struct emu_state *emu);

typedef struct hart_wait hart_wait_t;

struct hart_executor_ops {
    int (*start)(struct emu_state *emu);
    void (*request_stop)(struct emu_state *emu);
    void (*wake_hart)(struct emu_state *emu, uint32_t hart_id);
    int (*request_pause)(struct emu_state *emu, uint64_t pause_seq);
    int (*request_rfence)(struct emu_state *emu,
                          uint32_t hart_mask,
                          uint32_t hart_mask_base,
                          uint64_t rfence_seq);
    int (*join)(struct emu_state *emu);
};

struct hart_executor {
    enum hart_executor_backend backend;
    const struct hart_executor_ops *ops;
    void *backend_state;
    hart_executor_hart_thread_fn hart_thread_fn;
    hart_executor_io_thread_fn io_thread_fn;
    hart_executor_single_thread_fn single_thread_fn;
};

int hart_executor_init(struct emu_state *emu,
                       enum hart_executor_backend backend,
                       hart_executor_hart_thread_fn hart_thread_fn,
                       hart_executor_io_thread_fn io_thread_fn,
                       hart_executor_single_thread_fn single_thread_fn);
void hart_executor_destroy(struct emu_state *emu);
int hart_executor_start(struct emu_state *emu);
void hart_executor_request_stop(struct emu_state *emu);
void hart_executor_wake_hart(struct emu_state *emu, uint32_t hart_id);
void hart_executor_wake_all(struct emu_state *emu);
int hart_executor_request_pause(struct emu_state *emu, uint64_t pause_seq);
int hart_executor_request_rfence(struct emu_state *emu,
                                 uint32_t hart_mask,
                                 uint32_t hart_mask_base,
                                 uint64_t rfence_seq);
int hart_executor_join(struct emu_state *emu);
hart_wait_t *hart_executor_wait_for_hart(struct emu_state *emu,
                                         uint32_t hart_id);

void hart_executor_mark_fatal(struct emu_state *emu);
bool hart_executor_fatal(struct emu_state *emu);
