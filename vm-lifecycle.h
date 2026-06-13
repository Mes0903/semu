#ifndef SEMU_VM_LIFECYCLE_H
#define SEMU_VM_LIFECYCLE_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

enum semu_vm_lifecycle_state {
    SEMU_VM_CREATED,
    SEMU_VM_RUNNING,
    SEMU_VM_PAUSE_REQUESTED,
    SEMU_VM_PAUSED,
    SEMU_VM_DRAINING,
    SEMU_VM_RESETTING,
    SEMU_VM_STOPPING,
    SEMU_VM_STOPPED,
    SEMU_VM_FAILED,
};

struct semu_vm_lifecycle {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    enum semu_vm_lifecycle_state state;
    uint64_t generation;
    uint64_t pause_seq;
    uint64_t drain_seq;
    bool accepting_device_work;
};

int semu_vm_lifecycle_init(struct semu_vm_lifecycle *lc);
void semu_vm_lifecycle_destroy(struct semu_vm_lifecycle *lc);

enum semu_vm_lifecycle_state semu_vm_lifecycle_state(
    const struct semu_vm_lifecycle *lc);
uint64_t semu_vm_lifecycle_generation(const struct semu_vm_lifecycle *lc);
uint64_t semu_vm_lifecycle_pause_seq(const struct semu_vm_lifecycle *lc);
uint64_t semu_vm_lifecycle_drain_seq(const struct semu_vm_lifecycle *lc);
bool semu_vm_accepting_device_work(const struct semu_vm_lifecycle *lc);

int semu_vm_lifecycle_enter_running(struct semu_vm_lifecycle *lc);
int semu_vm_lifecycle_request_pause(struct semu_vm_lifecycle *lc);
int semu_vm_lifecycle_enter_paused(struct semu_vm_lifecycle *lc);
int semu_vm_lifecycle_enter_draining(struct semu_vm_lifecycle *lc);
int semu_vm_lifecycle_enter_resetting(struct semu_vm_lifecycle *lc);
int semu_vm_lifecycle_enter_stopping(struct semu_vm_lifecycle *lc);
int semu_vm_lifecycle_enter_stopped(struct semu_vm_lifecycle *lc);
int semu_vm_lifecycle_enter_failed(struct semu_vm_lifecycle *lc);

int semu_vm_lifecycle_wait_changed_from(struct semu_vm_lifecycle *lc,
                                        enum semu_vm_lifecycle_state state,
                                        enum semu_vm_lifecycle_state *observed);
int semu_vm_lifecycle_wait_until(struct semu_vm_lifecycle *lc,
                                 enum semu_vm_lifecycle_state target);

#endif
