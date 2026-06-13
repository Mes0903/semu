#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#ifdef MMU_CACHE_STATS
#include <sys/time.h>
#endif

#include "device.h"
#include "irq-source.h"
#include "mini-gdbstub/include/gdbstub.h"
#include "mmio-bus.h"
#include "platform.h"
#if SEMU_HAS(VIRTIOINPUT)
#include "virtio-input-event.h"
#endif
#if SEMU_HAS(VIRTIOGPU)
#include "vgpu-display.h"
#endif
#if SEMU_HAS(VIRTIOINPUT) || SEMU_HAS(VIRTIOGPU)
#include "window.h"
#endif
#include "riscv.h"
#include "riscv_private.h"

#define PRIV(x) ((emu_state_t *) x->priv)

#ifndef SEMU_PAUSE_ACK_TEST_HOOKS
#define SEMU_PAUSE_ACK_TEST_HOOKS 0
#endif

/* Forward declarations for runtime support */
typedef void (*semu_wfi_handler_t)(hart_t *hart);
static void UNUSED wfi_handler_single_hart(hart_t *hart);
static void wfi_handler_threaded(hart_t *hart);
static void *hart_thread_func(void *arg);
static void *io_thread_func(void *arg);
static bool UNUSED semu_should_use_threaded_runtime(const emu_state_t *emu);
static semu_wfi_handler_t semu_wfi_handler_for_config(const emu_state_t *emu);
static volatile sig_atomic_t signal_received = 0;
#if SEMU_PAUSE_ACK_TEST_HOOKS
static void (*semu_hsm_start_before_publish_hook)(emu_state_t *emu,
                                                  hart_t *target);
#endif
static void semu_process_pending_dma_invalidation(hart_t *hart);
static void semu_process_pending_rfence(hart_t *hart);
static bool semu_lifecycle_pause_active(enum semu_vm_lifecycle_state state);
static void semu_lifecycle_notify(emu_state_t *emu);
static void semu_process_hsm_resume_if_started(hart_t *hart);
static int semu_hart_pause_safe_point(hart_t *hart);
static int UNUSED semu_pause_all_harts(emu_state_t *emu);
static void semu_ram_dma_write_invalidate(void *opaque,
                                          guest_paddr_t addr,
                                          guest_size_t len);
static int semu_step_chunk(emu_state_t *emu, hart_t *hart, int steps);
static int semu_service_hart_step(emu_state_t *emu, hart_t *hart);
static int semu_run_chunk(emu_state_t *emu, int steps);

enum {
    SEMU_SMP_SLICE_STEPS = 8,
    SEMU_SMP_BATCH_STEPS = 4096,
    SEMU_SINGLE_SLICE_STEPS = 512,
    SEMU_SLIRP_SLICE_STEPS = 8,
};

static inline bool emu_stopped_load(const emu_state_t *emu)
{
    return __atomic_load_n(&emu->stopped, __ATOMIC_RELAXED);
}

static inline void emu_stopped_store(emu_state_t *emu, bool value)
{
    __atomic_store_n(&emu->stopped, value, __ATOMIC_RELAXED);
}

static inline bool emu_threaded_fatal_load(const emu_state_t *emu)
{
    return __atomic_load_n(&emu->threaded_fatal, __ATOMIC_RELAXED);
}

static inline void emu_threaded_fatal_store(emu_state_t *emu, bool value)
{
    __atomic_store_n(&emu->threaded_fatal, value, __ATOMIC_RELAXED);
}

static inline uint32_t emu_peripheral_update_ctr_load(emu_state_t *emu)
{
    return __atomic_load_n(&emu->peripheral_update_ctr, __ATOMIC_RELAXED);
}

static inline void emu_peripheral_update_ctr_store(emu_state_t *emu,
                                                   uint32_t value)
{
    __atomic_store_n(&emu->peripheral_update_ctr, value, __ATOMIC_RELAXED);
}

static bool UNUSED semu_should_use_threaded_runtime(const emu_state_t *emu)
{
    return emu->vm.n_hart > 1;
}

static semu_wfi_handler_t semu_wfi_handler_for_config(const emu_state_t *emu)
{
    if (semu_should_use_threaded_runtime(emu))
        return wfi_handler_threaded;
    return wfi_handler_single_hart;
}

static inline bool emu_peripheral_tick_due(emu_state_t *emu)
{
    for (;;) {
        uint32_t counter =
            __atomic_load_n(&emu->peripheral_update_ctr, __ATOMIC_RELAXED);
        if (counter == 0) {
            uint32_t expected = 0;
            if (__atomic_compare_exchange_n(&emu->peripheral_update_ctr,
                                            &expected, 64, false,
                                            __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
                return true;
            continue;
        }

        uint32_t next = counter - 1;
        if (__atomic_compare_exchange_n(&emu->peripheral_update_ctr, &counter,
                                        next, false, __ATOMIC_ACQ_REL,
                                        __ATOMIC_RELAXED))
            return false;
    }
}

static inline void emu_device_lock(pthread_mutex_t *lock)
{
    pthread_mutex_lock(lock);
}

static inline void emu_device_unlock(pthread_mutex_t *lock)
{
    pthread_mutex_unlock(lock);
}

#define EMU_DEVICE_CALL(lock, ...)  \
    do {                            \
        emu_device_lock(&(lock));   \
        __VA_ARGS__;                \
        emu_device_unlock(&(lock)); \
    } while (0)

static int emu_mutex_init(pthread_mutex_t *lock)
{
    int rc = pthread_mutex_init(lock, NULL);
    if (rc)
        errno = rc;
    return rc;
}

static void semu_signal_hart(emu_state_t *emu, uint32_t hartid)
{
    if (!emu->hart_wait || hartid >= emu->vm.n_hart)
        return;

    pthread_mutex_lock(&emu->hart_wait[hartid].mutex);
    pthread_cond_signal(&emu->hart_wait[hartid].cond);
    pthread_mutex_unlock(&emu->hart_wait[hartid].mutex);
}

static bool UNUSED semu_hart_has_enabled_interrupt(hart_t *hart)
{
    return (hart_sip_load(hart) & hart_sie_load(hart)) != 0;
}

static void UNUSED wfi_handler_single_hart(hart_t *hart)
{
    if (semu_hart_has_enabled_interrupt(hart))
        hart_in_wfi_store(hart, false);
}

static void UNUSED semu_resume_hart(emu_state_t *emu, uint32_t hartid)
{
    if (hartid >= emu->vm.n_hart)
        return;

    hart_t *hart = emu->vm.hart[hartid];
    bool resumed = false;

    pthread_mutex_lock(&emu->lifecycle.lock);
    if (!semu_lifecycle_pause_active(emu->lifecycle.state)) {
        int32_t expected = SBI_HSM_STATE_SUSPENDED;
        resumed = hart_hsm_status_compare_exchange(hart, &expected,
                                                   SBI_HSM_STATE_STARTED);
    }
    pthread_mutex_unlock(&emu->lifecycle.lock);

    if (resumed)
        semu_signal_hart(emu, hartid);
}

static void semu_pause_ack_locked(emu_state_t *emu,
                                  hart_t *hart,
                                  uint64_t pause_seq)
{
    if (pause_seq == 0)
        return;

    enum semu_vm_lifecycle_state state = emu->lifecycle.state;
    if (semu_lifecycle_pause_active(state) &&
        hart_pause_ack_seq_load(hart) < pause_seq) {
        hart_pause_ack(hart, pause_seq);
        pthread_cond_broadcast(&emu->lifecycle.cond);
    }
}

static void semu_finish_hsm_park(emu_state_t *emu, hart_t *hart)
{
    bool resume_after_suspend = false;

    semu_process_pending_rfence(hart);

    pthread_mutex_lock(&emu->lifecycle.lock);
    int32_t state = hart_hsm_status_load(hart);

    if (state == SBI_HSM_STATE_STOP_PENDING) {
        semu_pause_ack_locked(emu, hart, hart_pause_request_seq_load(hart));
        hart_hsm_status_store(hart, SBI_HSM_STATE_STOPPED);
        pthread_mutex_unlock(&emu->lifecycle.lock);
        return;
    }

    if (state == SBI_HSM_STATE_SUSPEND_PENDING) {
        semu_pause_ack_locked(emu, hart, hart_pause_request_seq_load(hart));
        hart_hsm_status_store(hart, SBI_HSM_STATE_SUSPENDED);
        resume_after_suspend = semu_hart_has_enabled_interrupt(hart);
    }
    pthread_mutex_unlock(&emu->lifecycle.lock);

    if (resume_after_suspend)
        semu_resume_hart(emu, hart->mhartid);
}

static void semu_signal_all_harts(emu_state_t *emu)
{
    for (uint32_t i = 0; i < emu->vm.n_hart; i++)
        semu_signal_hart(emu, i);
}

static bool semu_lifecycle_pause_active(enum semu_vm_lifecycle_state state)
{
    return state == SEMU_VM_PAUSE_REQUESTED || state == SEMU_VM_PAUSED;
}

static bool semu_lifecycle_terminal(enum semu_vm_lifecycle_state state)
{
    return state == SEMU_VM_STOPPING || state == SEMU_VM_STOPPED ||
           state == SEMU_VM_FAILED;
}

static void semu_lifecycle_notify(emu_state_t *emu)
{
    pthread_mutex_lock(&emu->lifecycle.lock);
    pthread_cond_broadcast(&emu->lifecycle.cond);
    pthread_mutex_unlock(&emu->lifecycle.lock);
}

static void semu_wake_hart_if_interrupt_pending(emu_state_t *emu,
                                                uint32_t hartid)
{
    if (hartid >= emu->vm.n_hart)
        return;

    if (semu_hart_has_enabled_interrupt(emu->vm.hart[hartid]))
        semu_resume_hart(emu, hartid);
}

void semu_wake_interruptible_harts(emu_state_t *emu)
{
    for (uint32_t i = 0; i < emu->vm.n_hart; i++)
        semu_wake_hart_if_interrupt_pending(emu, i);
}

static inline bool semu_rfence_initialized_load(emu_state_t *emu)
{
    return __atomic_load_n(&emu->rfence.initialized, __ATOMIC_ACQUIRE);
}

static inline void semu_rfence_initialized_store(emu_state_t *emu, bool value)
{
    __atomic_store_n(&emu->rfence.initialized, value, __ATOMIC_RELEASE);
}

static void semu_set_stopped(emu_state_t *emu, bool value)
{
    emu_stopped_store(emu, value);
    if (value) {
        semu_signal_all_harts(emu);
        semu_lifecycle_notify(emu);
        if (semu_rfence_initialized_load(emu)) {
            pthread_mutex_lock(&emu->rfence.completion_mutex);
            pthread_cond_broadcast(&emu->rfence.completion_cond);
            pthread_mutex_unlock(&emu->rfence.completion_mutex);
        }
    }
}

static inline int32_t semu_rfence_pending_count_load(emu_state_t *emu)
{
    return __atomic_load_n(&emu->rfence.pending_count, __ATOMIC_ACQUIRE);
}

static inline void semu_rfence_pending_count_store(emu_state_t *emu,
                                                   int32_t value)
{
    __atomic_store_n(&emu->rfence.pending_count, value, __ATOMIC_RELEASE);
}

static inline int semu_rfence_type_load(emu_state_t *emu)
{
    return __atomic_load_n(&emu->rfence.type, __ATOMIC_ACQUIRE);
}

static inline void semu_rfence_type_store(emu_state_t *emu, int type)
{
    __atomic_store_n(&emu->rfence.type, type, __ATOMIC_RELEASE);
}

static int semu_rfence_init(emu_state_t *emu)
{
    int rc;

    semu_rfence_initialized_store(emu, false);
    semu_rfence_type_store(emu, SEMU_RFENCE_NONE);
    semu_rfence_pending_count_store(emu, 0);
    rc = emu_mutex_init(&emu->rfence.issue_mutex);
    if (rc)
        return rc;
    rc = emu_mutex_init(&emu->rfence.completion_mutex);
    if (rc)
        return rc;
    rc = pthread_cond_init(&emu->rfence.completion_cond, NULL);
    if (rc) {
        errno = rc;
        return rc;
    }
    semu_rfence_initialized_store(emu, true);
    return 0;
}

static bool semu_rfence_targets_hart(uint32_t hartid,
                                     uint32_t hart_mask,
                                     uint32_t hart_mask_base)
{
    if (hart_mask_base == UINT32_MAX)
        return true;
    if (hartid < hart_mask_base)
        return false;

    uint32_t bit = hartid - hart_mask_base;
    if (bit >= 32)
        return false;
    return ((hart_mask >> bit) & 1U) != 0;
}

static void semu_apply_rfence(hart_t *hart,
                              int type,
                              uint32_t start_addr,
                              uint32_t size)
{
    switch (type) {
    case SEMU_RFENCE_I:
        mmu_invalidate(hart);
        break;
    case SEMU_RFENCE_VMA:
        mmu_invalidate_range(hart, start_addr, size);
        break;
    default:
        break;
    }
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

static void UNUSED semu_rfence_ack(emu_state_t *emu)
{
    int32_t old =
        __atomic_fetch_sub(&emu->rfence.pending_count, 1, __ATOMIC_ACQ_REL);
    if (old == 1) {
        pthread_mutex_lock(&emu->rfence.completion_mutex);
        pthread_cond_broadcast(&emu->rfence.completion_cond);
        pthread_mutex_unlock(&emu->rfence.completion_mutex);
    }
}

static void semu_process_pending_rfence(hart_t *hart)
{
    semu_process_pending_dma_invalidation(hart);

    if (!hart_pending_rfence_load(hart))
        return;

    emu_state_t *emu = PRIV(hart);
    int type = semu_rfence_type_load(emu);
    uint32_t start_addr = emu->rfence.start_addr;
    uint32_t size = emu->rfence.size;

    hart_pending_rfence_store(hart, false);
    semu_apply_rfence(hart, type, start_addr, size);
    semu_rfence_ack(emu);
}

static bool semu_pause_targets_complete_locked(emu_state_t *emu,
                                               const bool *targets,
                                               uint64_t pause_seq)
{
    for (uint32_t i = 0; i < emu->vm.n_hart; i++) {
        if (!targets[i])
            continue;
        if (hart_pause_ack_seq_load(emu->vm.hart[i]) < pause_seq)
            return false;
    }
    return true;
}

static int semu_pause_wait_for_targets(emu_state_t *emu,
                                       const bool *targets,
                                       uint64_t pause_seq)
{
    int ret = pthread_mutex_lock(&emu->lifecycle.lock);
    if (ret != 0)
        return -ret;

    while (!semu_pause_targets_complete_locked(emu, targets, pause_seq)) {
        enum semu_vm_lifecycle_state state = emu->lifecycle.state;
        if (emu_stopped_load(emu) || semu_lifecycle_terminal(state) ||
            !semu_lifecycle_pause_active(state)) {
            pthread_mutex_unlock(&emu->lifecycle.lock);
            return -ECANCELED;
        }

        ret = pthread_cond_wait(&emu->lifecycle.cond, &emu->lifecycle.lock);
        if (ret != 0) {
            pthread_mutex_unlock(&emu->lifecycle.lock);
            return -ret;
        }
    }

    ret = pthread_mutex_unlock(&emu->lifecycle.lock);
    if (ret != 0)
        return -ret;
    return 0;
}

static int UNUSED semu_pause_all_harts(emu_state_t *emu)
{
    if (!emu)
        return -EINVAL;

    uint32_t n_hart = emu->vm.n_hart;
    bool *targets = NULL;
    if (n_hart > 0) {
        targets = calloc(n_hart, sizeof(*targets));
        if (!targets)
            return -ENOMEM;
    }

    int ret = semu_vm_lifecycle_request_pause(&emu->lifecycle);
    if (ret < 0) {
        free(targets);
        return ret;
    }

    ret = pthread_mutex_lock(&emu->lifecycle.lock);
    if (ret != 0) {
        free(targets);
        return -ret;
    }

    uint64_t pause_seq = emu->lifecycle.pause_seq;
    for (uint32_t i = 0; i < n_hart; i++) {
        hart_t *target = emu->vm.hart[i];
        if (hart_hsm_status_load(target) != SBI_HSM_STATE_STARTED)
            continue;
        targets[i] = true;
        hart_pause_request(target, pause_seq);
    }
    ret = pthread_mutex_unlock(&emu->lifecycle.lock);
    if (ret != 0) {
        free(targets);
        return -ret;
    }

    for (uint32_t i = 0; i < n_hart; i++) {
        if (targets[i])
            semu_signal_hart(emu, i);
    }

    ret = semu_pause_wait_for_targets(emu, targets, pause_seq);
    free(targets);
    if (ret < 0)
        return ret;

    return semu_vm_lifecycle_enter_paused(&emu->lifecycle);
}

static void UNUSED semu_lock_rfence_issue(emu_state_t *emu, hart_t *requester)
{
    for (;;) {
        semu_process_pending_rfence(requester);
        if (pthread_mutex_trylock(&emu->rfence.issue_mutex) == 0)
            return;
        sched_yield();
    }
}

static void emu_update_plic_irq(emu_state_t *emu,
                                enum semu_irq_source source,
                                bool active)
{
    semu_irq_source_set(emu, source, active);
}

/* Define fetch separately since it is simpler (fixed width, already checked
 * alignment, only main RAM is executable).
 */
static void mem_fetch(hart_t *hart, uint32_t n_pages, uint32_t **page_addr)
{
    emu_state_t *data = PRIV(hart);
    if (unlikely(n_pages >= RAM_SIZE / RV_PAGE_SIZE)) {
        /* TODO: check for other regions */
        vm_set_exception(hart, RV_EXC_FETCH_FAULT, hart->exc_val);
        return;
    }
    *page_addr = &data->ram[n_pages << (RV_PAGE_SHIFT - 2)];
}

/* Similarly, only main memory pages can be used as page tables. */
static uint32_t *mem_page_table(const hart_t *hart, uint32_t ppn)
{
    emu_state_t *data = PRIV(hart);
    if (ppn < (RAM_SIZE / RV_PAGE_SIZE))
        return &data->ram[ppn << (RV_PAGE_SHIFT - 2)];
    return NULL;
}

static void UNUSED emu_update_uart_interrupts(vm_t *vm)
{
    emu_state_t *data = PRIV(vm->hart[0]);
    bool pending;

    EMU_DEVICE_CALL(data->uart_lock, u8250_update_interrupts(&data->uart);
                    pending = data->uart.pending_ints != 0);
    emu_update_plic_irq(data, SEMU_IRQ_SOURCE_UART, pending);
}

#if SEMU_HAS(VIRTIONET)
static void UNUSED emu_update_vnet_interrupts(vm_t *vm)
{
    emu_state_t *data = PRIV(vm->hart[0]);
    bool pending;

    EMU_DEVICE_CALL(data->vnet_lock, pending = data->vnet.InterruptStatus != 0);
    emu_update_plic_irq(data, SEMU_IRQ_SOURCE_VNET, pending);
}
#endif

#if SEMU_HAS(VIRTIOBLK)
static void UNUSED emu_update_vblk_interrupts(vm_t *vm)
{
    emu_state_t *data = PRIV(vm->hart[0]);
    bool pending;

    EMU_DEVICE_CALL(data->vblk_lock, pending = data->vblk.InterruptStatus != 0);
    emu_update_plic_irq(data, SEMU_IRQ_SOURCE_VBLK, pending);
}
#endif

#if SEMU_HAS(VIRTIORNG)
static void UNUSED emu_update_vrng_interrupts(vm_t *vm)
{
    emu_state_t *data = PRIV(vm->hart[0]);
    bool pending;

    EMU_DEVICE_CALL(data->vrng_lock, pending = data->vrng.InterruptStatus != 0);
    emu_update_plic_irq(data, SEMU_IRQ_SOURCE_VRNG, pending);
}
#endif

#if SEMU_HAS(VIRTIOINPUT)
static void UNUSED emu_update_vinput_keyboard_interrupts(vm_t *vm)
{
    emu_state_t *data = PRIV(vm->hart[0]);
    bool pending;

    EMU_DEVICE_CALL(data->vkeyboard_lock,
                    pending = virtio_input_irq_pending(&data->vkeyboard));
    emu_update_plic_irq(data, SEMU_IRQ_SOURCE_VINPUT_KEYBOARD, pending);
}

static void UNUSED emu_update_vinput_mouse_interrupts(vm_t *vm)
{
    emu_state_t *data = PRIV(vm->hart[0]);
    bool pending;

    EMU_DEVICE_CALL(data->vmouse_lock,
                    pending = virtio_input_irq_pending(&data->vmouse));
    emu_update_plic_irq(data, SEMU_IRQ_SOURCE_VINPUT_MOUSE, pending);
}
#endif

#if SEMU_HAS(VIRTIOGPU)
static void UNUSED emu_update_vgpu_interrupts(vm_t *vm)
{
    emu_state_t *data = PRIV(vm->hart[0]);
    bool pending;

    EMU_DEVICE_CALL(data->vgpu_lock,
                    pending = virtio_gpu_irq_pending(&data->vgpu));
    emu_update_plic_irq(data, SEMU_IRQ_SOURCE_VGPU, pending);
}
#endif

static void emu_update_timer_interrupt(hart_t *hart)
{
    emu_state_t *data = PRIV(hart);

    aclint_mtimer_update_interrupts(hart, &data->mtimer);
}

static void emu_update_swi_interrupt(hart_t *hart)
{
    emu_state_t *data = PRIV(hart);
    aclint_swi_update_interrupts(hart, &data->mswi, &data->sswi);
}

#if SEMU_HAS(VIRTIOSND)
static void UNUSED emu_update_vsnd_interrupts(vm_t *vm)
{
    emu_state_t *data = PRIV(vm->hart[0]);
    bool pending;

    EMU_DEVICE_CALL(data->vsnd_lock,
                    pending = __atomic_load_n(&data->vsnd.InterruptStatus,
                                              __ATOMIC_ACQUIRE) != 0);
    emu_update_plic_irq(data, SEMU_IRQ_SOURCE_VSND, pending);
}
#endif

#if SEMU_HAS(VIRTIOFS)
static void UNUSED emu_update_vfs_interrupts(vm_t *vm)
{
    emu_state_t *data = PRIV(vm->hart[0]);
    bool pending;

    EMU_DEVICE_CALL(data->vfs_lock, pending = data->vfs.InterruptStatus != 0);
    emu_update_plic_irq(data, SEMU_IRQ_SOURCE_VFS, pending);
}
#endif

/* Peripheral I/O polling strategy
 *
 * We use inline polling instead of dedicated I/O coroutines for peripherals.
 *
 * Rationale:
 * 1. Non-blocking poll() is extremely cheap (~200ns syscall overhead)
 * 2. Inline polling provides lowest latency (checked every SMP slice)
 * 3. All harts share peripheral_update_ctr, ensuring frequent polling
 *    regardless of hart count (e.g., 4 harts = 4 polls per 256 instructions)
 * 4. Coroutine-based I/O would INCREASE latency by n_hart factor due to
 *    scheduler round-robin, without reducing poll() overhead meaningfully
 *
 * Coroutines are reserved for hart scheduling where they provide real value:
 * - Enable event-driven WFI (avoid busy-wait when guest is idle)
 * - Support SBI HSM (Hart State Management) for dynamic hart start/stop
 * - Provide clean abstraction for multi-hart execution
 *
 * For simple non-blocking I/O, inline polling is superior.
 */
static void io_poll_peripherals(emu_state_t *emu)
{
    bool pending;

    EMU_DEVICE_CALL(emu->uart_lock, u8250_check_ready(&emu->uart);
                    u8250_flush_out(&emu->uart);
                    u8250_update_interrupts(&emu->uart);
                    pending = emu->uart.pending_ints != 0);
    emu_update_plic_irq(emu, SEMU_IRQ_SOURCE_UART, pending);

#if SEMU_HAS(VIRTIONET)
    EMU_DEVICE_CALL(emu->vnet_lock, virtio_net_refresh_queue(&emu->vnet);
                    pending = emu->vnet.InterruptStatus != 0);
    emu_update_plic_irq(emu, SEMU_IRQ_SOURCE_VNET, pending);
#endif

#if SEMU_HAS(VIRTIOBLK)
    EMU_DEVICE_CALL(emu->vblk_lock, pending = emu->vblk.InterruptStatus != 0);
    emu_update_plic_irq(emu, SEMU_IRQ_SOURCE_VBLK, pending);
#endif

#if SEMU_HAS(VIRTIORNG)
    EMU_DEVICE_CALL(emu->vrng_lock, pending = emu->vrng.InterruptStatus != 0);
    emu_update_plic_irq(emu, SEMU_IRQ_SOURCE_VRNG, pending);
#endif

#if SEMU_HAS(VIRTIOSND)
    EMU_DEVICE_CALL(emu->vsnd_lock,
                    pending = __atomic_load_n(&emu->vsnd.InterruptStatus,
                                              __ATOMIC_ACQUIRE) != 0);
    emu_update_plic_irq(emu, SEMU_IRQ_SOURCE_VSND, pending);
#endif

#if SEMU_HAS(VIRTIOFS)
    EMU_DEVICE_CALL(emu->vfs_lock, pending = emu->vfs.InterruptStatus != 0);
    emu_update_plic_irq(emu, SEMU_IRQ_SOURCE_VFS, pending);
#endif
#if SEMU_HAS(VIRTIOINPUT)
    /* The empty path is common during CI and boot workloads, so only
     * drain the host-side queue after the window thread has published
     * pending work for the emulator thread.
     */
    if (vinput_may_have_pending_cmds()) {
        emu_device_lock(&emu->vkeyboard_lock);
        emu_device_lock(&emu->vmouse_lock);
        virtio_input_drain_host_events();
        emu_device_unlock(&emu->vmouse_lock);
        emu_device_unlock(&emu->vkeyboard_lock);
    }

    EMU_DEVICE_CALL(emu->vkeyboard_lock,
                    pending = virtio_input_irq_pending(&emu->vkeyboard));
    emu_update_plic_irq(emu, SEMU_IRQ_SOURCE_VINPUT_KEYBOARD, pending);

    EMU_DEVICE_CALL(emu->vmouse_lock,
                    pending = virtio_input_irq_pending(&emu->vmouse));
    emu_update_plic_irq(emu, SEMU_IRQ_SOURCE_VINPUT_MOUSE, pending);
#endif
#if SEMU_HAS(VIRTIOINPUT) || SEMU_HAS(VIRTIOGPU)
    /* A closed window is treated like a frontend shutdown request. */
    if (g_window.window_is_closed())
        semu_set_stopped(emu, true);
#endif
}

static inline void emu_tick_peripherals(emu_state_t *emu)
{
    if (emu_peripheral_tick_due(emu))
        io_poll_peripherals(emu);
}

typedef bool (*semu_runtime_mmio_read_fn)(hart_t *hart,
                                          void *opaque,
                                          uint64_t off,
                                          uint8_t width,
                                          uint32_t *value);
typedef bool (*semu_runtime_mmio_write_fn)(hart_t *hart,
                                           void *opaque,
                                           uint64_t off,
                                           uint8_t width,
                                           uint32_t value);

static void semu_runtime_mmio_set_callbacks(struct semu_mmio_region *region,
                                            void *opaque,
                                            semu_runtime_mmio_read_fn read,
                                            semu_runtime_mmio_write_fn write)
{
    region->opaque = opaque;
    region->read = read;
    region->write = write;
}

static uint32_t semu_plic_legacy_offset(uint64_t window_base, uint64_t off)
{
    return (uint32_t) ((window_base & UINT64_C(0x03ffffff)) + off);
}

static bool semu_mmio_plic_read_window(hart_t *hart,
                                       void *opaque,
                                       uint64_t off,
                                       uint8_t width,
                                       uint32_t *value,
                                       uint64_t window_base)
{
    emu_state_t *data = opaque;
    uint32_t plic_addr = semu_plic_legacy_offset(window_base, off);

    EMU_DEVICE_CALL(data->plic_lock,
                    plic_read(hart, &data->plic, plic_addr, width, value);
                    plic_update_interrupts(hart->vm, &data->plic));
    semu_wake_interruptible_harts(data);
    return true;
}

static bool semu_mmio_plic_write_window(hart_t *hart,
                                        void *opaque,
                                        uint64_t off,
                                        uint8_t width,
                                        uint32_t value,
                                        uint64_t window_base)
{
    emu_state_t *data = opaque;
    uint32_t plic_addr = semu_plic_legacy_offset(window_base, off);

    EMU_DEVICE_CALL(data->plic_lock,
                    plic_write(hart, &data->plic, plic_addr, width, value);
                    plic_update_interrupts(hart->vm, &data->plic));
    semu_wake_interruptible_harts(data);
    return true;
}

static bool semu_mmio_plic_window0_read(hart_t *hart,
                                        void *opaque,
                                        uint64_t off,
                                        uint8_t width,
                                        uint32_t *value)
{
    return semu_mmio_plic_read_window(hart, opaque, off, width, value,
                                      SEMU_PLATFORM_MMIO_PLIC_WINDOW0_BASE);
}

static bool semu_mmio_plic_window0_write(hart_t *hart,
                                         void *opaque,
                                         uint64_t off,
                                         uint8_t width,
                                         uint32_t value)
{
    return semu_mmio_plic_write_window(hart, opaque, off, width, value,
                                       SEMU_PLATFORM_MMIO_PLIC_WINDOW0_BASE);
}

static bool semu_mmio_plic_window2_read(hart_t *hart,
                                        void *opaque,
                                        uint64_t off,
                                        uint8_t width,
                                        uint32_t *value)
{
    return semu_mmio_plic_read_window(hart, opaque, off, width, value,
                                      SEMU_PLATFORM_MMIO_PLIC_WINDOW2_BASE);
}

static bool semu_mmio_plic_window2_write(hart_t *hart,
                                         void *opaque,
                                         uint64_t off,
                                         uint8_t width,
                                         uint32_t value)
{
    return semu_mmio_plic_write_window(hart, opaque, off, width, value,
                                       SEMU_PLATFORM_MMIO_PLIC_WINDOW2_BASE);
}

static bool semu_mmio_uart_read(hart_t *hart,
                                void *opaque,
                                uint64_t off,
                                uint8_t width,
                                uint32_t *value)
{
    emu_state_t *data = opaque;
    bool pending;

    EMU_DEVICE_CALL(data->uart_lock,
                    u8250_read(hart, &data->uart, (uint32_t) off, width, value);
                    u8250_update_interrupts(&data->uart);
                    pending = data->uart.pending_ints != 0);
    emu_update_plic_irq(data, SEMU_IRQ_SOURCE_UART, pending);
    return true;
}

static bool semu_mmio_uart_write(hart_t *hart,
                                 void *opaque,
                                 uint64_t off,
                                 uint8_t width,
                                 uint32_t value)
{
    emu_state_t *data = opaque;
    bool pending;

    EMU_DEVICE_CALL(data->uart_lock, u8250_write(hart, &data->uart,
                                                 (uint32_t) off, width, value);
                    u8250_update_interrupts(&data->uart);
                    pending = data->uart.pending_ints != 0);
    emu_update_plic_irq(data, SEMU_IRQ_SOURCE_UART, pending);
    return true;
}

#if SEMU_HAS(VIRTIONET)
static bool semu_mmio_vnet_read(hart_t *hart,
                                void *opaque,
                                uint64_t off,
                                uint8_t width,
                                uint32_t *value)
{
    emu_state_t *data = opaque;

    EMU_DEVICE_CALL(
        data->vnet_lock,
        virtio_net_read(hart, &data->vnet, (uint32_t) off, width, value));
    return true;
}

static bool semu_mmio_vnet_write(hart_t *hart,
                                 void *opaque,
                                 uint64_t off,
                                 uint8_t width,
                                 uint32_t value)
{
    emu_state_t *data = opaque;
    bool pending;

    EMU_DEVICE_CALL(
        data->vnet_lock,
        virtio_net_write(hart, &data->vnet, (uint32_t) off, width, value);
        pending = data->vnet.InterruptStatus != 0);
    emu_update_plic_irq(data, SEMU_IRQ_SOURCE_VNET, pending);
    return true;
}
#endif

#if SEMU_HAS(VIRTIOBLK)
static bool semu_mmio_vblk_read(hart_t *hart,
                                void *opaque,
                                uint64_t off,
                                uint8_t width,
                                uint32_t *value)
{
    emu_state_t *data = opaque;

    EMU_DEVICE_CALL(
        data->vblk_lock,
        virtio_blk_read(hart, &data->vblk, (uint32_t) off, width, value));
    return true;
}

static bool semu_mmio_vblk_write(hart_t *hart,
                                 void *opaque,
                                 uint64_t off,
                                 uint8_t width,
                                 uint32_t value)
{
    emu_state_t *data = opaque;
    bool pending;

    EMU_DEVICE_CALL(
        data->vblk_lock,
        virtio_blk_write(hart, &data->vblk, (uint32_t) off, width, value);
        pending = data->vblk.InterruptStatus != 0);
    emu_update_plic_irq(data, SEMU_IRQ_SOURCE_VBLK, pending);
    return true;
}
#endif

static bool semu_mmio_mtimer_read(hart_t *hart,
                                  void *opaque,
                                  uint64_t off,
                                  uint8_t width,
                                  uint32_t *value)
{
    emu_state_t *data = opaque;

    EMU_DEVICE_CALL(
        data->mtimer_lock,
        aclint_mtimer_read(hart, &data->mtimer, (uint32_t) off, width, value));
    return true;
}

static bool semu_mmio_mtimer_write(hart_t *hart,
                                   void *opaque,
                                   uint64_t off,
                                   uint8_t width,
                                   uint32_t value)
{
    emu_state_t *data = opaque;
    uint32_t mtimer_addr = (uint32_t) off;
    uint32_t target_hartid = mtimer_addr >> 3;
    bool update_all_timers = mtimer_addr >= 0x7FF8 && mtimer_addr < 0x8000;

    EMU_DEVICE_CALL(
        data->mtimer_lock,
        aclint_mtimer_write(hart, &data->mtimer, mtimer_addr, width, value));
    if (hart->error)
        return true;

    if (update_all_timers) {
        for (uint32_t i = 0; i < hart->vm->n_hart; i++) {
            emu_update_timer_interrupt(hart->vm->hart[i]);
            semu_wake_hart_if_interrupt_pending(data, i);
        }
    } else if (target_hartid < hart->vm->n_hart) {
        emu_update_timer_interrupt(hart->vm->hart[target_hartid]);
        semu_wake_hart_if_interrupt_pending(data, target_hartid);
    }
    return true;
}

static bool semu_mmio_mswi_read(hart_t *hart,
                                void *opaque,
                                uint64_t off,
                                uint8_t width,
                                uint32_t *value)
{
    emu_state_t *data = opaque;

    EMU_DEVICE_CALL(
        data->mswi_lock,
        aclint_mswi_read(hart, &data->mswi, (uint32_t) off, width, value));
    return true;
}

static bool semu_mmio_mswi_write(hart_t *hart,
                                 void *opaque,
                                 uint64_t off,
                                 uint8_t width,
                                 uint32_t value)
{
    emu_state_t *data = opaque;
    uint32_t target_hartid = ((uint32_t) off) >> 2;

    EMU_DEVICE_CALL(
        data->mswi_lock,
        aclint_mswi_write(hart, &data->mswi, (uint32_t) off, width, value);
        aclint_swi_update_interrupts(hart, &data->mswi, &data->sswi));
    if (target_hartid < hart->vm->n_hart)
        emu_update_swi_interrupt(hart->vm->hart[target_hartid]);
    semu_wake_hart_if_interrupt_pending(data, target_hartid);
    return true;
}

static bool semu_mmio_sswi_read(hart_t *hart,
                                void *opaque,
                                uint64_t off,
                                uint8_t width,
                                uint32_t *value)
{
    emu_state_t *data = opaque;

    EMU_DEVICE_CALL(
        data->sswi_lock,
        aclint_sswi_read(hart, &data->sswi, (uint32_t) off, width, value));
    return true;
}

static bool semu_mmio_sswi_write(hart_t *hart,
                                 void *opaque,
                                 uint64_t off,
                                 uint8_t width,
                                 uint32_t value)
{
    emu_state_t *data = opaque;
    uint32_t target_hartid = ((uint32_t) off) >> 2;

    EMU_DEVICE_CALL(
        data->sswi_lock,
        aclint_sswi_write(hart, &data->sswi, (uint32_t) off, width, value);
        aclint_swi_update_interrupts(hart, &data->mswi, &data->sswi));
    if (target_hartid < hart->vm->n_hart)
        emu_update_swi_interrupt(hart->vm->hart[target_hartid]);
    semu_wake_hart_if_interrupt_pending(data, target_hartid);
    return true;
}

#if SEMU_HAS(VIRTIORNG)
static bool semu_mmio_vrng_read(hart_t *hart,
                                void *opaque,
                                uint64_t off,
                                uint8_t width,
                                uint32_t *value)
{
    emu_state_t *data = opaque;

    EMU_DEVICE_CALL(
        data->vrng_lock,
        virtio_rng_read(hart, &data->vrng, (uint32_t) off, width, value));
    return true;
}

static bool semu_mmio_vrng_write(hart_t *hart,
                                 void *opaque,
                                 uint64_t off,
                                 uint8_t width,
                                 uint32_t value)
{
    emu_state_t *data = opaque;
    bool pending;

    EMU_DEVICE_CALL(
        data->vrng_lock,
        virtio_rng_write(hart, &data->vrng, (uint32_t) off, width, value);
        pending = data->vrng.InterruptStatus != 0);
    emu_update_plic_irq(data, SEMU_IRQ_SOURCE_VRNG, pending);
    return true;
}
#endif

#if SEMU_HAS(VIRTIOSND)
static bool semu_mmio_vsnd_read(hart_t *hart,
                                void *opaque,
                                uint64_t off,
                                uint8_t width,
                                uint32_t *value)
{
    emu_state_t *data = opaque;

    EMU_DEVICE_CALL(
        data->vsnd_lock,
        virtio_snd_read(hart, &data->vsnd, (uint32_t) off, width, value));
    return true;
}

static bool semu_mmio_vsnd_write(hart_t *hart,
                                 void *opaque,
                                 uint64_t off,
                                 uint8_t width,
                                 uint32_t value)
{
    emu_state_t *data = opaque;
    bool pending;

    EMU_DEVICE_CALL(
        data->vsnd_lock,
        virtio_snd_write(hart, &data->vsnd, (uint32_t) off, width, value);
        pending = __atomic_load_n(&data->vsnd.InterruptStatus,
                                  __ATOMIC_ACQUIRE) != 0);
    emu_update_plic_irq(data, SEMU_IRQ_SOURCE_VSND, pending);
    return true;
}
#endif

#if SEMU_HAS(VIRTIOFS)
static bool semu_mmio_vfs_read(hart_t *hart,
                               void *opaque,
                               uint64_t off,
                               uint8_t width,
                               uint32_t *value)
{
    emu_state_t *data = opaque;

    EMU_DEVICE_CALL(
        data->vfs_lock,
        virtio_fs_read(hart, &data->vfs, (uint32_t) off, width, value));
    return true;
}

static bool semu_mmio_vfs_write(hart_t *hart,
                                void *opaque,
                                uint64_t off,
                                uint8_t width,
                                uint32_t value)
{
    emu_state_t *data = opaque;
    bool pending;

    EMU_DEVICE_CALL(
        data->vfs_lock,
        virtio_fs_write(hart, &data->vfs, (uint32_t) off, width, value);
        pending = data->vfs.InterruptStatus != 0);
    emu_update_plic_irq(data, SEMU_IRQ_SOURCE_VFS, pending);
    return true;
}
#endif

#if SEMU_HAS(VIRTIOINPUT)
static bool semu_mmio_vkeyboard_read(hart_t *hart,
                                     void *opaque,
                                     uint64_t off,
                                     uint8_t width,
                                     uint32_t *value)
{
    emu_state_t *data = opaque;

    EMU_DEVICE_CALL(data->vkeyboard_lock,
                    virtio_input_read(hart, &data->vkeyboard, (uint32_t) off,
                                      width, value));
    return true;
}

static bool semu_mmio_vkeyboard_write(hart_t *hart,
                                      void *opaque,
                                      uint64_t off,
                                      uint8_t width,
                                      uint32_t value)
{
    emu_state_t *data = opaque;
    bool pending;

    EMU_DEVICE_CALL(data->vkeyboard_lock,
                    virtio_input_write(hart, &data->vkeyboard, (uint32_t) off,
                                       width, value);
                    pending = virtio_input_irq_pending(&data->vkeyboard));
    emu_update_plic_irq(data, SEMU_IRQ_SOURCE_VINPUT_KEYBOARD, pending);
    return true;
}

static bool semu_mmio_vmouse_read(hart_t *hart,
                                  void *opaque,
                                  uint64_t off,
                                  uint8_t width,
                                  uint32_t *value)
{
    emu_state_t *data = opaque;

    EMU_DEVICE_CALL(
        data->vmouse_lock,
        virtio_input_read(hart, &data->vmouse, (uint32_t) off, width, value));
    return true;
}

static bool semu_mmio_vmouse_write(hart_t *hart,
                                   void *opaque,
                                   uint64_t off,
                                   uint8_t width,
                                   uint32_t value)
{
    emu_state_t *data = opaque;
    bool pending;

    EMU_DEVICE_CALL(
        data->vmouse_lock,
        virtio_input_write(hart, &data->vmouse, (uint32_t) off, width, value);
        pending = virtio_input_irq_pending(&data->vmouse));
    emu_update_plic_irq(data, SEMU_IRQ_SOURCE_VINPUT_MOUSE, pending);
    return true;
}
#endif

#if SEMU_HAS(VIRTIOGPU)
static bool semu_mmio_vgpu_read(hart_t *hart,
                                void *opaque,
                                uint64_t off,
                                uint8_t width,
                                uint32_t *value)
{
    emu_state_t *data = opaque;

    EMU_DEVICE_CALL(
        data->vgpu_lock,
        virtio_gpu_read(hart, &data->vgpu, (uint32_t) off, width, value));
    return true;
}

static bool semu_mmio_vgpu_write(hart_t *hart,
                                 void *opaque,
                                 uint64_t off,
                                 uint8_t width,
                                 uint32_t value)
{
    emu_state_t *data = opaque;
    bool pending;

    EMU_DEVICE_CALL(
        data->vgpu_lock,
        virtio_gpu_write(hart, &data->vgpu, (uint32_t) off, width, value);
        pending = virtio_gpu_irq_pending(&data->vgpu));
    emu_update_plic_irq(data, SEMU_IRQ_SOURCE_VGPU, pending);
    return true;
}
#endif

static void semu_configure_runtime_mmio(
    const struct semu_platform_device *device,
    struct semu_mmio_region *region,
    void *opaque)
{
    switch (device->base) {
    case SEMU_PLATFORM_MMIO_PLIC_WINDOW0_BASE:
        semu_runtime_mmio_set_callbacks(region, opaque,
                                        semu_mmio_plic_window0_read,
                                        semu_mmio_plic_window0_write);
        break;
    case SEMU_PLATFORM_MMIO_PLIC_WINDOW2_BASE:
        semu_runtime_mmio_set_callbacks(region, opaque,
                                        semu_mmio_plic_window2_read,
                                        semu_mmio_plic_window2_write);
        break;
    case SEMU_PLATFORM_MMIO_UART_BASE:
        semu_runtime_mmio_set_callbacks(region, opaque, semu_mmio_uart_read,
                                        semu_mmio_uart_write);
        break;
#if SEMU_HAS(VIRTIONET)
    case SEMU_PLATFORM_MMIO_VNET_BASE:
        semu_runtime_mmio_set_callbacks(region, opaque, semu_mmio_vnet_read,
                                        semu_mmio_vnet_write);
        break;
#endif
#if SEMU_HAS(VIRTIOBLK)
    case SEMU_PLATFORM_MMIO_VBLK_BASE:
        semu_runtime_mmio_set_callbacks(region, opaque, semu_mmio_vblk_read,
                                        semu_mmio_vblk_write);
        break;
#endif
    case SEMU_PLATFORM_MMIO_MTIMER_BASE:
        semu_runtime_mmio_set_callbacks(region, opaque, semu_mmio_mtimer_read,
                                        semu_mmio_mtimer_write);
        break;
    case SEMU_PLATFORM_MMIO_MSWI_BASE:
        semu_runtime_mmio_set_callbacks(region, opaque, semu_mmio_mswi_read,
                                        semu_mmio_mswi_write);
        break;
    case SEMU_PLATFORM_MMIO_SSWI_BASE:
        semu_runtime_mmio_set_callbacks(region, opaque, semu_mmio_sswi_read,
                                        semu_mmio_sswi_write);
        break;
#if SEMU_HAS(VIRTIORNG)
    case SEMU_PLATFORM_MMIO_VRNG_BASE:
        semu_runtime_mmio_set_callbacks(region, opaque, semu_mmio_vrng_read,
                                        semu_mmio_vrng_write);
        break;
#endif
#if SEMU_HAS(VIRTIOSND)
    case SEMU_PLATFORM_MMIO_VSND_BASE:
        semu_runtime_mmio_set_callbacks(region, opaque, semu_mmio_vsnd_read,
                                        semu_mmio_vsnd_write);
        break;
#endif
#if SEMU_HAS(VIRTIOFS)
    case SEMU_PLATFORM_MMIO_VFS_BASE:
        semu_runtime_mmio_set_callbacks(region, opaque, semu_mmio_vfs_read,
                                        semu_mmio_vfs_write);
        break;
#endif
#if SEMU_HAS(VIRTIOINPUT)
    case SEMU_PLATFORM_MMIO_VINPUT_KEYBOARD_BASE:
        semu_runtime_mmio_set_callbacks(region, opaque,
                                        semu_mmio_vkeyboard_read,
                                        semu_mmio_vkeyboard_write);
        break;
    case SEMU_PLATFORM_MMIO_VINPUT_MOUSE_BASE:
        semu_runtime_mmio_set_callbacks(region, opaque, semu_mmio_vmouse_read,
                                        semu_mmio_vmouse_write);
        break;
#endif
#if SEMU_HAS(VIRTIOGPU)
    case SEMU_PLATFORM_MMIO_VGPU_BASE:
        semu_runtime_mmio_set_callbacks(region, opaque, semu_mmio_vgpu_read,
                                        semu_mmio_vgpu_write);
        break;
#endif
    default:
        break;
    }
}

static bool semu_register_runtime_mmio(emu_state_t *emu)
{
    semu_mmio_bus_init(&emu->mmio_bus);
    return semu_platform_register_fixed_mmio_configured(
        &emu->mmio_bus, semu_configure_runtime_mmio, emu);
}

static void mem_load(hart_t *hart,
                     uint32_t addr,
                     uint8_t width,
                     uint32_t *value)
{
    emu_state_t *data = PRIV(hart);

    /* RAM at 0x00000000 + RAM_SIZE */
    if (addr < RAM_SIZE) {
        ram_read(hart, data->ram, addr, width, value);
        return;
    }

    if (semu_mmio_bus_read(&data->mmio_bus, hart, addr, width, value))
        return;

    vm_set_exception(hart, RV_EXC_LOAD_FAULT, hart->exc_val);
}

static void mem_store(hart_t *hart,
                      uint32_t addr,
                      uint8_t width,
                      uint32_t value)
{
    emu_state_t *data = PRIV(hart);

    /* RAM at 0x00000000 + RAM_SIZE */
    if (addr < RAM_SIZE) {
        ram_write(hart, data->ram, addr, width, value);
        return;
    }

    if (semu_mmio_bus_write(&data->mmio_bus, hart, addr, width, value))
        return;

    vm_set_exception(hart, RV_EXC_STORE_FAULT, hart->exc_val);
}

/* SBI */
#define SBI_IMPL_ID 0x999
#define SBI_IMPL_VERSION 1

typedef struct {
    int32_t error;
    int32_t value;
} sbi_ret_t;

static inline sbi_ret_t handle_sbi_ecall_TIMER(hart_t *hart, int32_t fid)
{
    emu_state_t *data = PRIV(hart);
    switch (fid) {
    case SBI_TIMER__SET_TIMER:
        __atomic_store_n(&data->mtimer.mtimecmp[hart->mhartid],
                         (((uint64_t) hart->x_regs[RV_R_A1]) << 32) |
                             (uint64_t) (hart->x_regs[RV_R_A0]),
                         __ATOMIC_RELAXED);
        hart_sip_clear_bits(hart, RV_INT_STI_BIT);
        emu_update_timer_interrupt(hart);
        semu_wake_hart_if_interrupt_pending(data, hart->mhartid);
        return (sbi_ret_t) {SBI_SUCCESS, 0};
    default:
        return (sbi_ret_t) {SBI_ERR_NOT_SUPPORTED, 0};
    }
}

static inline sbi_ret_t handle_sbi_ecall_RST(hart_t *hart, int32_t fid)
{
    emu_state_t *data = PRIV(hart);
    switch (fid) {
    case SBI_RST__SYSTEM_RESET:
        fprintf(stderr, "system reset: type=%u, reason=%u\n",
                hart->x_regs[RV_R_A0], hart->x_regs[RV_R_A1]);
        semu_set_stopped(data, true);
        return (sbi_ret_t) {SBI_SUCCESS, 0};
    default:
        return (sbi_ret_t) {SBI_ERR_NOT_SUPPORTED, 0};
    }
}

static inline sbi_ret_t handle_sbi_ecall_HSM(hart_t *hart, int32_t fid)
{
    uint32_t hartid, start_addr, opaque, suspend_type, resume_addr;
    emu_state_t *emu = PRIV(hart);
    vm_t *vm = hart->vm;
    switch (fid) {
    case SBI_HSM__HART_START: {
        hartid = hart->x_regs[RV_R_A0];
        if (hartid >= vm->n_hart)
            return (sbi_ret_t) {SBI_ERR_INVALID_PARAM, 0};

        hart_t *target = vm->hart[hartid];
        start_addr = hart->x_regs[RV_R_A1];
        opaque = hart->x_regs[RV_R_A2];

        pthread_mutex_lock(&emu->lifecycle.lock);
        if (semu_lifecycle_pause_active(emu->lifecycle.state)) {
            pthread_mutex_unlock(&emu->lifecycle.lock);
            return (sbi_ret_t) {SBI_ERR_FAILED, 0};
        }

        int32_t expected = SBI_HSM_STATE_STOPPED;
        if (!hart_hsm_status_compare_exchange(target, &expected,
                                              SBI_HSM_STATE_START_PENDING)) {
            pthread_mutex_unlock(&emu->lifecycle.lock);
            return (sbi_ret_t) {SBI_ERR_ALREADY_AVAILABLE, 0};
        }

        mmu_set_satp(target, 0);
        target->sstatus_sie = false;
        target->x_regs[RV_R_A0] = hartid;
        target->x_regs[RV_R_A1] = opaque;
        target->pc = start_addr;
        target->s_mode = true;
        hart_hsm_resume_pending_store(target, false);
#if SEMU_PAUSE_ACK_TEST_HOOKS
        if (semu_hsm_start_before_publish_hook)
            semu_hsm_start_before_publish_hook(emu, target);
#endif
        hart_hsm_status_store(target, SBI_HSM_STATE_STARTED);
        pthread_mutex_unlock(&emu->lifecycle.lock);

        semu_signal_hart(emu, hartid);
        return (sbi_ret_t) {SBI_SUCCESS, 0};
    }
    case SBI_HSM__HART_STOP:
        hart_hsm_status_store(hart, SBI_HSM_STATE_STOP_PENDING);
        hart->error = ERR_USER;
        return (sbi_ret_t) {SBI_SUCCESS, 0};
    case SBI_HSM__HART_GET_STATUS:
        hartid = hart->x_regs[RV_R_A0];
        if (hartid >= vm->n_hart)
            return (sbi_ret_t) {SBI_ERR_INVALID_PARAM, 0};
        return (sbi_ret_t) {SBI_SUCCESS,
                            hart_hsm_status_load(vm->hart[hartid])};
    case SBI_HSM__HART_SUSPEND:
        suspend_type = hart->x_regs[RV_R_A0];
        resume_addr = hart->x_regs[RV_R_A1];
        opaque = hart->x_regs[RV_R_A2];
        if (suspend_type == 0x00000000) {
            hart->hsm_resume_is_ret = true;
            hart->hsm_resume_pc = hart->pc;
        } else if (suspend_type == 0x80000000) {
            hart->hsm_resume_is_ret = false;
            hart->hsm_resume_pc = resume_addr;
            hart->hsm_resume_opaque = opaque;
        } else {
            return (sbi_ret_t) {SBI_ERR_INVALID_PARAM, 0};
        }
        hart_hsm_resume_pending_store(hart, true);
        hart_hsm_status_store(hart, SBI_HSM_STATE_SUSPEND_PENDING);
        hart->error = ERR_USER;
        return (sbi_ret_t) {SBI_SUCCESS, 0};
    default:
        return (sbi_ret_t) {SBI_ERR_NOT_SUPPORTED, 0};
    }
    return (sbi_ret_t) {SBI_ERR_FAILED, 0};
}

static inline sbi_ret_t handle_sbi_ecall_IPI(hart_t *hart, int32_t fid)
{
    emu_state_t *data = PRIV(hart);
    uint32_t hart_mask, hart_mask_base;
    switch (fid) {
    case SBI_IPI__SEND_IPI:
        hart_mask = hart->x_regs[RV_R_A0];
        hart_mask_base = hart->x_regs[RV_R_A1];
        if (hart_mask_base == UINT32_MAX) {
            for (uint32_t i = 0; i < hart->vm->n_hart; i++) {
                __atomic_store_n(&data->sswi.ssip[i], 1, __ATOMIC_RELAXED);
                emu_update_swi_interrupt(data->vm.hart[i]);
                semu_wake_hart_if_interrupt_pending(data, i);
            }
        } else {
            for (uint32_t i = hart_mask_base; hart_mask && i < hart->vm->n_hart;
                 hart_mask >>= 1, i++) {
                if (hart_mask & 1) {
                    __atomic_store_n(&data->sswi.ssip[i], 1, __ATOMIC_RELAXED);
                    emu_update_swi_interrupt(data->vm.hart[i]);
                    semu_wake_hart_if_interrupt_pending(data, i);
                }
            }
        }

        return (sbi_ret_t) {SBI_SUCCESS, 0};
        break;
    default:
        return (sbi_ret_t) {SBI_ERR_FAILED, 0};
    }
}

static sbi_ret_t UNUSED semu_rfence_threaded(hart_t *hart,
                                             int type,
                                             uint32_t hart_mask,
                                             uint32_t hart_mask_base,
                                             uint32_t start_addr,
                                             uint32_t size,
                                             uint32_t asid)
{
    emu_state_t *emu = PRIV(hart);
    vm_t *vm = hart->vm;
    int32_t pending_count = 0;
    bool pending_targets[vm->n_hart];

    memset(pending_targets, 0, sizeof(pending_targets));

    semu_lock_rfence_issue(emu, hart);

    emu->rfence.start_addr = start_addr;
    emu->rfence.size = size;
    emu->rfence.asid = asid;
    semu_rfence_type_store(emu, type);

    for (uint32_t i = 0; i < vm->n_hart; i++) {
        hart_t *target = vm->hart[i];
        if (!semu_rfence_targets_hart(i, hart_mask, hart_mask_base))
            continue;

        if (i == hart->mhartid ||
            hart_hsm_status_load(target) != SBI_HSM_STATE_STARTED) {
            semu_apply_rfence(target, type, start_addr, size);
            continue;
        }

        pending_targets[i] = true;
        pending_count++;
    }

    semu_rfence_pending_count_store(emu, pending_count);
    for (uint32_t i = 0; i < vm->n_hart; i++) {
        if (!pending_targets[i])
            continue;
        hart_pending_rfence_store(vm->hart[i], true);
        semu_signal_hart(emu, i);
    }
    if (pending_count > 0)
        semu_lifecycle_notify(emu);

    pthread_mutex_lock(&emu->rfence.completion_mutex);
    while (semu_rfence_pending_count_load(emu) > 0 && !emu_stopped_load(emu)) {
        pthread_cond_wait(&emu->rfence.completion_cond,
                          &emu->rfence.completion_mutex);
    }
    bool stopped = semu_rfence_pending_count_load(emu) > 0;
    pthread_mutex_unlock(&emu->rfence.completion_mutex);

    if (!stopped)
        semu_rfence_type_store(emu, SEMU_RFENCE_NONE);
    pthread_mutex_unlock(&emu->rfence.issue_mutex);
    return (sbi_ret_t) {stopped ? SBI_ERR_FAILED : SBI_SUCCESS, 0};
}

static sbi_ret_t UNUSED semu_rfence_direct(hart_t *hart,
                                           int type,
                                           uint32_t hart_mask,
                                           uint32_t hart_mask_base,
                                           uint32_t start_addr,
                                           uint32_t size)
{
    for (uint32_t i = 0; i < hart->vm->n_hart; i++) {
        if (semu_rfence_targets_hart(i, hart_mask, hart_mask_base))
            semu_apply_rfence(hart->vm->hart[i], type, start_addr, size);
    }
    return (sbi_ret_t) {SBI_SUCCESS, 0};
}

static sbi_ret_t semu_rfence_request(hart_t *hart,
                                     int type,
                                     uint32_t hart_mask,
                                     uint32_t hart_mask_base,
                                     uint32_t start_addr,
                                     uint32_t size,
                                     uint32_t asid)
{
    return semu_rfence_threaded(hart, type, hart_mask, hart_mask_base,
                                start_addr, size, asid);
}

static void semu_process_pending_dma_invalidation(hart_t *hart)
{
    if (!hart || !hart->vm)
        return;

    uint64_t generation = __atomic_load_n(
        &hart->vm->dma_write_invalidate_generation, __ATOMIC_ACQUIRE);
    if (hart->dma_write_invalidate_generation_seen == generation)
        return;

    hart->dma_write_invalidate_generation_seen = generation;
    semu_apply_rfence(hart, SEMU_RFENCE_I, 0, (uint32_t) -1);
}

static void semu_ram_dma_write_invalidate(void *opaque,
                                          guest_paddr_t addr,
                                          guest_size_t len)
{
    emu_state_t *emu = opaque;

    if (!emu || len == 0)
        return;

    /* DMA reports GPA ranges, while current hart caches are VA keyed. Publish
     * a VM-wide generation and let harts invalidate locally at safe points.
     */
    (void) addr;
    __atomic_fetch_add(&emu->vm.dma_write_invalidate_generation, 1,
                       __ATOMIC_RELEASE);
    semu_signal_all_harts(emu);
}

static inline sbi_ret_t handle_sbi_ecall_RFENCE(hart_t *hart, int32_t fid)
{
    uint32_t hart_mask, hart_mask_base;
    uint32_t start_addr, size, asid;
    switch (fid) {
    case SBI_RFENCE__I:
        hart_mask = hart->x_regs[RV_R_A0];
        hart_mask_base = hart->x_regs[RV_R_A1];
        return semu_rfence_request(hart, SEMU_RFENCE_I, hart_mask,
                                   hart_mask_base, 0, (uint32_t) -1, 0);
    case SBI_RFENCE__VMA:
        hart_mask = hart->x_regs[RV_R_A0];
        hart_mask_base = hart->x_regs[RV_R_A1];
        start_addr = hart->x_regs[RV_R_A2];
        size = hart->x_regs[RV_R_A3];
        return semu_rfence_request(hart, SEMU_RFENCE_VMA, hart_mask,
                                   hart_mask_base, start_addr, size, 0);
    case SBI_RFENCE__VMA_ASID:
        hart_mask = hart->x_regs[RV_R_A0];
        hart_mask_base = hart->x_regs[RV_R_A1];
        start_addr = hart->x_regs[RV_R_A2];
        size = hart->x_regs[RV_R_A3];
        asid = hart->x_regs[RV_R_A4];
        return semu_rfence_request(hart, SEMU_RFENCE_VMA, hart_mask,
                                   hart_mask_base, start_addr, size, asid);
    case SBI_RFENCE__GVMA_VMID:
    case SBI_RFENCE__GVMA:
    case SBI_RFENCE__VVMA_ASID:
    case SBI_RFENCE__VVMA:
        /* Hypervisor-related RFENCE operations - not implemented */
        return (sbi_ret_t) {SBI_SUCCESS, 0};
    default:
        return (sbi_ret_t) {SBI_ERR_FAILED, 0};
    }
}


#define RV_MVENDORID 0x12345678
#define RV_MARCHID ((1ULL << 31) | 1)
#define RV_MIMPID 1

static inline sbi_ret_t handle_sbi_ecall_BASE(hart_t *hart, int32_t fid)
{
    switch (fid) {
    case SBI_BASE__GET_SBI_IMPL_ID:
        return (sbi_ret_t) {SBI_SUCCESS, SBI_IMPL_ID};
    case SBI_BASE__GET_SBI_IMPL_VERSION:
        return (sbi_ret_t) {SBI_SUCCESS, SBI_IMPL_VERSION};
    case SBI_BASE__GET_MVENDORID:
        return (sbi_ret_t) {SBI_SUCCESS, RV_MVENDORID};
    case SBI_BASE__GET_MARCHID:
        return (sbi_ret_t) {SBI_SUCCESS, RV_MARCHID};
    case SBI_BASE__GET_MIMPID:
        return (sbi_ret_t) {SBI_SUCCESS, RV_MIMPID};
    case SBI_BASE__GET_SBI_SPEC_VERSION:
        return (sbi_ret_t) {SBI_SUCCESS, (2 << 24) | 0}; /* version 2.0 */
    case SBI_BASE__PROBE_EXTENSION: {
        int32_t eid = (int32_t) hart->x_regs[RV_R_A0];
        bool available = eid == SBI_EID_BASE || eid == SBI_EID_TIMER ||
                         eid == SBI_EID_RST || eid == SBI_EID_HSM ||
                         eid == SBI_EID_IPI || eid == SBI_EID_RFENCE;
        return (sbi_ret_t) {SBI_SUCCESS, available};
    }
    default:
        return (sbi_ret_t) {SBI_ERR_NOT_SUPPORTED, 0};
    }
}

#define SBI_HANDLE(TYPE) \
    ret = handle_sbi_ecall_##TYPE(hart, hart->x_regs[RV_R_A6])

static void handle_sbi_ecall(hart_t *hart)
{
    sbi_ret_t ret;
    switch (hart->x_regs[RV_R_A7]) {
    case SBI_EID_BASE:
        SBI_HANDLE(BASE);
        break;
    case SBI_EID_TIMER:
        SBI_HANDLE(TIMER);
        break;
    case SBI_EID_RST:
        SBI_HANDLE(RST);
        break;
    case SBI_EID_HSM:
        SBI_HANDLE(HSM);
        break;
    case SBI_EID_IPI:
        SBI_HANDLE(IPI);
        break;
    case SBI_EID_RFENCE:
        SBI_HANDLE(RFENCE);
        break;
    default:
        ret = (sbi_ret_t) {SBI_ERR_NOT_SUPPORTED, 0};
    }
    hart->x_regs[RV_R_A0] = (uint32_t) ret.error;
    hart->x_regs[RV_R_A1] = (uint32_t) ret.value;

    if (hart->error == ERR_USER)
        return;

    /* Clear error to allow execution to continue */
    hart->error = ERR_NONE;
}

#define N_MAPPERS 4

struct mapper {
    char *addr;
    uint32_t size;
};

static struct mapper mapper[N_MAPPERS] = {0};
static int map_index = 0;
static void unmap_files(void)
{
    while (map_index--) {
        if (!mapper[map_index].addr)
            continue;
        munmap(mapper[map_index].addr, mapper[map_index].size);
    }
}

static void map_file(char **ram_loc, const char *name)
{
    int fd = open(name, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "could not open %s\n", name);
        exit(2);
    }

    /* get file size */
    struct stat st;
    fstat(fd, &st);

    /* remap to a memory region */
    *ram_loc = mmap(*ram_loc, st.st_size, PROT_READ | PROT_WRITE,
                    MAP_FIXED | MAP_PRIVATE, fd, 0);
    if (*ram_loc == MAP_FAILED) {
        perror("mmap");
        close(fd);
        exit(2);
    }

    mapper[map_index].addr = *ram_loc;
    mapper[map_index].size = st.st_size;
    map_index++;

    /* The kernel selects a nearby page boundary and attempts to create
     * the mapping.
     */
    *ram_loc += st.st_size;

    close(fd);
}

static void usage(const char *execpath)
{
    fprintf(stderr,
            "Usage: %s -k linux-image [-b dtb] [-i initrd-image] [-d "
            "disk-image] [-s shared-directory] [-H]\n",
            execpath);
}

static void handle_options(int argc,
                           char **argv,
                           char **kernel_file,
                           char **dtb_file,
                           char **initrd_file,
                           char **disk_file,
                           char **net_dev,
                           int *hart_count,
                           bool *debug,
                           bool *headless,
                           char **shared_dir)
{
    *kernel_file = *dtb_file = *initrd_file = *disk_file = *net_dev =
        *shared_dir = NULL;

    int optidx = 0;
    struct option opts[] = {
        {"kernel", 1, NULL, 'k'},     {"dtb", 1, NULL, 'b'},
        {"initrd", 1, NULL, 'i'},     {"disk", 1, NULL, 'd'},
        {"netdev", 1, NULL, 'n'},     {"smp", 1, NULL, 'c'},
        {"gdbstub", 0, NULL, 'g'},    {"help", 0, NULL, 'h'},
        {"shared_dir", 1, NULL, 's'}, {"headless", 0, NULL, 'H'}};

    int c;
    while ((c = getopt_long(argc, argv, "k:b:i:d:n:c:s:ghH", opts, &optidx)) !=
           -1) {
        switch (c) {
        case 'k':
            *kernel_file = optarg;
            break;
        case 'b':
            *dtb_file = optarg;
            break;
        case 'i':
            *initrd_file = optarg;
            break;
        case 'd':
            *disk_file = optarg;
            break;
        case 'n':
            *net_dev = optarg;
            break;
        case 'c': {
            /* strtol over atoi: well-defined behavior on overflow plus
             * trailing-junk detection. Upper bound is 32 because
             * plic_state_t.ie[] is sized for 32 contexts (one per hart);
             * see device.h.
             */
            char *end;
            errno = 0;
            long n = strtol(optarg, &end, 10);
            if (errno || *end || end == optarg || n < 1 || n > 32) {
                fprintf(stderr,
                        "%s: -c expects an integer hart count in [1,32], "
                        "got '%s'\n",
                        argv[0], optarg);
                exit(2);
            }
            *hart_count = (int) n;
            break;
        }
        case 's':
            *shared_dir = optarg;
            break;
        case 'g':
            *debug = true;
            break;
        case 'H':
            *headless = true;
            break;
        case 'h':
            usage(argv[0]);
            exit(0);
        default:
            break;
        }
    }

    if (!*kernel_file) {
        fprintf(stderr,
                "Linux kernel image file must "
                "be provided via -k option.\n");
        usage(argv[0]);
        exit(2);
    }

    if (!*dtb_file)
        *dtb_file = "minimal.dtb";
}

static bool uses_default_minimal_dtb(const char *dtb_file)
{
    const char *name;

    if (!dtb_file)
        return false;

    name = strrchr(dtb_file, '/');
    if (name)
        name++;
    else
        name = dtb_file;

    return strcmp(name, "minimal.dtb") == 0;
}

static int validate_default_dtb_boot_options(const char *dtb_file,
                                             const char *initrd_file,
                                             const char *disk_file)
{
    if (!uses_default_minimal_dtb(dtb_file)) {
        fprintf(stderr,
                "note: skipping default minimal.dtb boot option checks "
                "for custom DTB '%s'.\n",
                dtb_file ? dtb_file : "(null)");
        return 0;
    }

#if SEMU_HAS(EXTERNAL_ROOT)
    if (initrd_file) {
        fprintf(stderr,
                "-i requires a DTB with linux,initrd-start/end. "
                "Rebuild with ENABLE_EXTERNAL_ROOT=0 or pass -b "
                "<dtb-with-initrd>.\n");
        return 2;
    }

    if (!disk_file) {
        fprintf(stderr,
                "warning: EXTERNAL_ROOT build expects -d <disk-image>; "
                "without it the kernel will hang in rootwait.\n");
    }
#else
    if (!initrd_file) {
        fprintf(stderr,
                "This semu build uses legacy initramfs boot "
                "(ENABLE_EXTERNAL_ROOT=0).\n"
                "Run with -i rootfs.cpio, use make check, or rebuild "
                "with ENABLE_EXTERNAL_ROOT=1 for /dev/vda root boot.\n");
        if (disk_file) {
            fprintf(stderr,
                    "-d only attaches a virtio-blk disk; it does not "
                    "select the root filesystem without root=/dev/vda "
                    "in DTB bootargs.\n");
        }
        return 2;
    }
#endif

    return 0;
}

#define INIT_HART(hart, emu, id)                            \
    do {                                                    \
        hart->priv = emu;                                   \
        hart->mhartid = id;                                 \
        hart->ram_base = (emu)->ram;                        \
        hart->ram_size = RAM_SIZE;                          \
        hart->mem_fetch = mem_fetch;                        \
        hart->mem_load = mem_load;                          \
        hart->mem_store = mem_store;                        \
        hart->mem_page_table = mem_page_table;              \
        hart->s_mode = true;                                \
        hart_hsm_status_store(hart, SBI_HSM_STATE_STOPPED); \
        vm_init(hart);                                      \
    } while (0)

static int semu_init(emu_state_t *emu, int argc, char **argv)
{
    char *kernel_file;
    char *dtb_file;
    char *initrd_file;
    char *disk_file;
    char *netdev;
    char *shared_dir;
    int hart_count = 1;
    bool debug = false;
    bool headless = false;
    vm_t *vm = &emu->vm;
    handle_options(argc, argv, &kernel_file, &dtb_file, &initrd_file,
                   &disk_file, &netdev, &hart_count, &debug, &headless,
                   &shared_dir);
#if !SEMU_HAS(VIRTIOINPUT) && !SEMU_HAS(VIRTIOGPU)
    (void) headless;
#endif

    int boot_option_status =
        validate_default_dtb_boot_options(dtb_file, initrd_file, disk_file);
    if (boot_option_status)
        return boot_option_status;

    /* Initialize the emulator */
    memset(emu, 0, sizeof(*emu));

    int lifecycle_ret = semu_vm_lifecycle_init(&emu->lifecycle);
    if (lifecycle_ret < 0) {
        errno = -lifecycle_ret;
        perror("semu_vm_lifecycle_init");
        return 1;
    }

#define INIT_EMU_MUTEX(lock)              \
    do {                                  \
        if (emu_mutex_init(&(lock))) {    \
            perror("pthread_mutex_init"); \
            return 1;                     \
        }                                 \
    } while (0)

    INIT_EMU_MUTEX(emu->plic_lock);
    INIT_EMU_MUTEX(emu->uart_lock);
    INIT_EMU_MUTEX(emu->mtimer_lock);
    INIT_EMU_MUTEX(emu->mswi_lock);
    INIT_EMU_MUTEX(emu->sswi_lock);
#if SEMU_HAS(VIRTIONET)
    INIT_EMU_MUTEX(emu->vnet_lock);
#endif
#if SEMU_HAS(VIRTIOBLK)
    INIT_EMU_MUTEX(emu->vblk_lock);
#endif
#if SEMU_HAS(VIRTIORNG)
    INIT_EMU_MUTEX(emu->vrng_lock);
#endif
#if SEMU_HAS(VIRTIOSND)
    INIT_EMU_MUTEX(emu->vsnd_lock);
#endif
#if SEMU_HAS(VIRTIOFS)
    INIT_EMU_MUTEX(emu->vfs_lock);
#endif
#if SEMU_HAS(VIRTIOINPUT)
    INIT_EMU_MUTEX(emu->vkeyboard_lock);
    INIT_EMU_MUTEX(emu->vmouse_lock);
#endif
#if SEMU_HAS(VIRTIOGPU)
    INIT_EMU_MUTEX(emu->vgpu_lock);
#endif
#undef INIT_EMU_MUTEX

    if (!semu_register_runtime_mmio(emu)) {
        fprintf(stderr, "Failed to register runtime MMIO bus.\n");
        return 1;
    }

    /* Set up RAM */
    emu->ram = mmap(NULL, RAM_SIZE, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (emu->ram == MAP_FAILED) {
        fprintf(stderr, "Could not map RAM\n");
        return 2;
    }
    assert(!(((uintptr_t) emu->ram) & 0b11));

    /* Memory layout. Two shapes depending on whether `-i` was given:
     *
     *   Default (vda boot, no -i):
     *     0                                         RAM_SIZE - 1MiB
     *     +------------------+--------//-----+-----+
     *     |   kernel image   |   free RAM    | dtb |
     *     +------------------+--------//-----+-----+
     *                                               (dtb at top)
     *
     *   Legacy initramfs (-i present):
     *     0                       RAM-9MiB  RAM-1MiB
     *     +------------------+----+---------+-----+
     *     |   kernel image   | .. | initrd  | dtb |
     *     +------------------+----+---------+-----+
     *                              (8 MiB)  (1 MiB)
     *
     * dtb sits in the last 1 MiB and initrd, when present, in the 8 MiB
     * just below it -- both placements keep the kernel from clobbering
     * them as it allocates downward from RAM_SIZE.
     */
    char *ram_loc = (char *) emu->ram;
    /* Load Linux kernel image at the base of RAM */
    map_file(&ram_loc, kernel_file);
    /* Load dtb at the last 1 MiB so the kernel will not overwrite it */
    uint32_t dtb_addr = RAM_SIZE - DTB_SIZE;
    ram_loc = ((char *) emu->ram) + dtb_addr;
    map_file(&ram_loc, dtb_file);
    /* Load optional initrd image in the 8 MiB just below the dtb region
     * (legacy boot path; not used when the guest boots from /dev/vda).
     */
    if (initrd_file) {
        uint32_t initrd_addr = dtb_addr - INITRD_SIZE;
        ram_loc = ((char *) emu->ram) + initrd_addr;
        map_file(&ram_loc, initrd_file);
    }

    /* Hook for unmapping files */
    atexit(unmap_files);

    /* Set up RISC-V harts */
    vm->n_hart = hart_count;
    vm->hart = malloc(sizeof(hart_t *) * vm->n_hart);
    if (!vm->hart) {
        fprintf(stderr, "Failed to allocate %u hart slots.\n", vm->n_hart);
        return 1;
    }
    vm->reservations = calloc(vm->n_hart, sizeof(*vm->reservations));
    if (!vm->reservations) {
        fprintf(stderr, "Failed to allocate %u reservation slots.\n",
                vm->n_hart);
        return 1;
    }
    __atomic_store_n(&vm->any_reservation_active, false, __ATOMIC_RELAXED);
    if (semu_rfence_init(emu)) {
        perror("rfence init");
        return 1;
    }
    if (pthread_mutex_init(&vm->reservation_lock, NULL)) {
        perror("reservation lock init");
        return 1;
    }

    ram_dma_init(&emu->ram_dma, emu->ram, RAM_SIZE, vm);
    ram_dma_set_write_invalidate_callback(&emu->ram_dma,
                                          semu_ram_dma_write_invalidate, emu);

    emu->hart_wait = calloc(vm->n_hart, sizeof(*emu->hart_wait));
    emu->hart_threads = calloc(vm->n_hart, sizeof(*emu->hart_threads));
    if (!emu->hart_wait || !emu->hart_threads) {
        fprintf(stderr, "Failed to allocate threaded hart state.\n");
        return 1;
    }
    for (uint32_t i = 0; i < vm->n_hart; i++) {
        if (emu_mutex_init(&emu->hart_wait[i].mutex) ||
            pthread_cond_init(&emu->hart_wait[i].cond, NULL)) {
            perror("threaded hart wait init");
            return 1;
        }
    }
    for (uint32_t i = 0; i < vm->n_hart; i++) {
        hart_t *newhart = calloc(1, sizeof(hart_t));
        if (!newhart) {
            fprintf(stderr, "Failed to allocate hart #%u.\n", i);
            return 1;
        }
        INIT_HART(newhart, emu, i);
        newhart->x_regs[RV_R_A0] = i;
        newhart->x_regs[RV_R_A1] = dtb_addr;
        if (i == 0) {
            hart_hsm_status_store(newhart, SBI_HSM_STATE_STARTED);
            /* Set initial PC for hart 0 to kernel entry point (semu RAM base at
             * 0x0) */
            newhart->pc = 0x00000000;
        }

        newhart->vm = vm;
        newhart->wfi = semu_wfi_handler_for_config(emu);
        vm->hart[i] = newhart;
    }

    /* Set up peripherals */
    emu->uart.in_fd = 0, emu->uart.out_fd = 1;
    capture_keyboard_input(); /* set up uart */
#if SEMU_HAS(VIRTIONET)
    /* Always set ram pointer, even if netdev is not configured.
     * Device tree may still expose the device to guest.
     */
    emu->vnet.ram = emu->ram;
    if (netdev) {
        if (!virtio_net_init(&emu->vnet, netdev)) {
            fprintf(stderr, "Failed to initialize virtio-net device.\n");
            return 1;
        }
    }
#endif
#if SEMU_HAS(VIRTIOBLK)
    emu->vblk.ram = emu->ram;
    emu->disk = virtio_blk_init(&(emu->vblk), disk_file);
#endif
#if SEMU_HAS(VIRTIORNG)
    emu->vrng.ram = emu->ram;
    virtio_rng_init();
#endif
    /* Set up ACLINT */
    semu_timer_init(&emu->mtimer.mtime, CLOCK_FREQ, hart_count);
    emu->mtimer.mtimecmp = calloc(vm->n_hart, sizeof(*emu->mtimer.mtimecmp));
    emu->mtimer.n_hart = vm->n_hart;
    emu->mswi.msip = calloc(vm->n_hart, sizeof(*emu->mswi.msip));
    emu->mswi.n_hart = vm->n_hart;
    emu->sswi.ssip = calloc(vm->n_hart, sizeof(*emu->sswi.ssip));
    emu->sswi.n_hart = vm->n_hart;
#if SEMU_HAS(VIRTIOSND)
    if (!virtio_snd_init(&(emu->vsnd)))
        fprintf(stderr, "No virtio-snd functioned\n");
    emu->vsnd.ram = emu->ram;
#endif
#if SEMU_HAS(VIRTIOFS)
    emu->vfs.ram = emu->ram;
    if (!virtio_fs_init(&(emu->vfs), "myfs", shared_dir))
        fprintf(stderr, "No virtio-fs functioned\n");
#endif

#if SEMU_HAS(VIRTIOINPUT)
    virtio_input_init(&(emu->vkeyboard), emu, SEMU_IRQ_SOURCE_VINPUT_KEYBOARD);

    virtio_input_init(&(emu->vmouse), emu, SEMU_IRQ_SOURCE_VINPUT_MOUSE);
#endif

#if SEMU_HAS(VIRTIOGPU)
    virtio_gpu_init(&(emu->vgpu), emu);
    uint32_t scanout_id =
        virtio_gpu_register_scanout(&(emu->vgpu), SCREEN_WIDTH, SCREEN_HEIGHT);
    vgpu_display_set_scanout_count(scanout_id + 1U);
#endif

#if SEMU_HAS(VIRTIOINPUT) || SEMU_HAS(VIRTIOGPU)
    g_window.window_init(headless, SCREEN_WIDTH, SCREEN_HEIGHT);

    emu->wake_fd[0] = emu->wake_fd[1] = -1;
    if (vm->n_hart > 1 && g_window.window_main_loop) {
        if (pipe(emu->wake_fd) < 0) {
            perror("failed to create emulator wake pipe");
#if SEMU_HAS(VIRTIOGPU)
            virtio_gpu_destroy(&emu->vgpu);
            vgpu_display_shutdown_after_producer_stopped();
#endif
            g_window.window_cleanup();
            return EXIT_FAILURE;
        }

        /* Make the write end non-blocking so 'window_shutdown_sw()' never
         * stalls. The read end remains blocking because 'semu_run()' reads it
         * only after 'poll()' reports 'POLLIN' on the same emulator thread.
         */
        int flags = fcntl(emu->wake_fd[1], F_GETFL, 0);
        if (flags < 0 ||
            fcntl(emu->wake_fd[1], F_SETFL, flags | O_NONBLOCK) < 0) {
            perror(
                "failed to configure emulator wake pipe write end as "
                "non-blocking");
            close(emu->wake_fd[0]);
            close(emu->wake_fd[1]);
            emu->wake_fd[0] = emu->wake_fd[1] = -1;
#if SEMU_HAS(VIRTIOGPU)
            virtio_gpu_destroy(&emu->vgpu);
            vgpu_display_shutdown_after_producer_stopped();
#endif
            g_window.window_cleanup();
            return EXIT_FAILURE;
        }
    }
#endif

    emu_peripheral_update_ctr_store(emu, 0);
    emu->debug = debug;

    return 0;
}

static void wfi_handler_threaded(hart_t *hart)
{
    emu_state_t *emu = PRIV(hart);
    uint32_t id = hart->mhartid;

    emu_update_timer_interrupt(hart);
    emu_update_swi_interrupt(hart);
    if (hart_sip_load(hart) & hart_sie_load(hart))
        return;

    pthread_mutex_lock(&emu->hart_wait[id].mutex);
    hart_in_wfi_store(hart, true);
    while (!emu_stopped_load(emu) &&
           hart_hsm_status_load(hart) == SBI_HSM_STATE_STARTED) {
        pthread_mutex_unlock(&emu->hart_wait[id].mutex);
        semu_process_pending_rfence(hart);
        if (semu_hart_pause_safe_point(hart) < 0) {
            pthread_mutex_lock(&emu->hart_wait[id].mutex);
            break;
        }
        emu_update_timer_interrupt(hart);
        emu_update_swi_interrupt(hart);
        pthread_mutex_lock(&emu->hart_wait[id].mutex);

        if (hart_pending_rfence_load(hart))
            continue;
        if (hart_sip_load(hart) & hart_sie_load(hart))
            break;

        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_nsec += 1000000;
        if (deadline.tv_nsec >= 1000000000) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000;
        }
        pthread_cond_timedwait(&emu->hart_wait[id].cond,
                               &emu->hart_wait[id].mutex, &deadline);
    }
    hart_in_wfi_store(hart, false);
    pthread_mutex_unlock(&emu->hart_wait[id].mutex);
}

static void handle_hsm_resume(hart_t *hart)
{
    if (!hart_hsm_resume_pending_load(hart))
        return;

    if (hart->hsm_resume_is_ret) {
        hart_hsm_resume_pending_store(hart, false);
        return;
    }

    mmu_set_satp(hart, 0);
    hart->sstatus_sie = false;
    hart->s_mode = true;
    hart->x_regs[RV_R_A0] = hart->mhartid;
    hart->x_regs[RV_R_A1] = hart->hsm_resume_opaque;
    hart->pc = hart->hsm_resume_pc;
    hart_hsm_resume_pending_store(hart, false);
}

static void semu_process_hsm_resume_if_started(hart_t *hart)
{
    if (hart_hsm_status_load(hart) == SBI_HSM_STATE_STARTED)
        handle_hsm_resume(hart);
}

static int semu_hart_pause_safe_point(hart_t *hart)
{
    if (!hart || !hart->priv)
        return 0;
    if (hart_hsm_status_load(hart) != SBI_HSM_STATE_STARTED)
        return 0;

    emu_state_t *emu = PRIV(hart);
    uint64_t pause_seq = hart_pause_request_seq_load(hart);
    if (pause_seq == 0)
        return 0;

    int ret = pthread_mutex_lock(&emu->lifecycle.lock);
    if (ret != 0)
        return -ret;

    for (;;) {
        enum semu_vm_lifecycle_state state = emu->lifecycle.state;
        if (emu_stopped_load(emu) || semu_lifecycle_terminal(state)) {
            pthread_mutex_unlock(&emu->lifecycle.lock);
            return -ECANCELED;
        }
        if (!semu_lifecycle_pause_active(state)) {
            pthread_mutex_unlock(&emu->lifecycle.lock);
            return 0;
        }
        if (hart_hsm_status_load(hart) != SBI_HSM_STATE_STARTED) {
            pthread_mutex_unlock(&emu->lifecycle.lock);
            return 0;
        }

        pause_seq = hart_pause_request_seq_load(hart);
        if (pause_seq != 0 && hart_pause_ack_seq_load(hart) < pause_seq) {
            pthread_mutex_unlock(&emu->lifecycle.lock);
            semu_process_pending_rfence(hart);
            semu_process_hsm_resume_if_started(hart);

            ret = pthread_mutex_lock(&emu->lifecycle.lock);
            if (ret != 0)
                return -ret;
            state = emu->lifecycle.state;
            if (!emu_stopped_load(emu) && semu_lifecycle_pause_active(state) &&
                !semu_lifecycle_terminal(state) &&
                hart_hsm_status_load(hart) == SBI_HSM_STATE_STARTED &&
                hart_pause_ack_seq_load(hart) < pause_seq) {
                hart_pause_ack(hart, pause_seq);
                pthread_cond_broadcast(&emu->lifecycle.cond);
            }
            continue;
        }

        pthread_mutex_unlock(&emu->lifecycle.lock);
        semu_process_pending_rfence(hart);

        ret = pthread_mutex_lock(&emu->lifecycle.lock);
        if (ret != 0)
            return -ret;

        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_nsec += 1000000;
        if (deadline.tv_nsec >= 1000000000) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000;
        }
        ret = pthread_cond_timedwait(&emu->lifecycle.cond, &emu->lifecycle.lock,
                                     &deadline);
        if (ret != 0 && ret != ETIMEDOUT) {
            pthread_mutex_unlock(&emu->lifecycle.lock);
            return -ret;
        }
    }
}

static void wait_for_hart_start(emu_state_t *emu,
                                hart_t *hart,
                                int32_t initial_state)
{
    (void) initial_state;
    uint32_t id = hart->mhartid;

    pthread_mutex_lock(&emu->hart_wait[id].mutex);
    while (!emu_stopped_load(emu) &&
           hart_hsm_status_load(hart) != SBI_HSM_STATE_STARTED) {
        if (hart_pending_rfence_load(hart)) {
            pthread_mutex_unlock(&emu->hart_wait[id].mutex);
            semu_process_pending_rfence(hart);
            pthread_mutex_lock(&emu->hart_wait[id].mutex);
            continue;
        }
        pthread_cond_wait(&emu->hart_wait[id].cond, &emu->hart_wait[id].mutex);
    }
    pthread_mutex_unlock(&emu->hart_wait[id].mutex);

    if (!emu_stopped_load(emu))
        semu_process_hsm_resume_if_started(hart);
}

static void *hart_thread_func(void *arg)
{
    hart_t *hart = (hart_t *) arg;
    emu_state_t *emu = PRIV(hart);

    while (!emu_stopped_load(emu)) {
        if (signal_received) {
            semu_set_stopped(emu, true);
            break;
        }

        int32_t state = hart_hsm_status_load(hart);
        if (state != SBI_HSM_STATE_STARTED) {
            wait_for_hart_start(emu, hart, state);
            continue;
        }
        semu_process_hsm_resume_if_started(hart);
        if (semu_hart_pause_safe_point(hart) < 0)
            break;

        for (int i = 0; i < SEMU_SMP_BATCH_STEPS && !emu_stopped_load(emu);
             i += SEMU_SMP_SLICE_STEPS) {
            semu_process_pending_rfence(hart);
            if (semu_hart_pause_safe_point(hart) < 0)
                break;
            if (hart_hsm_status_load(hart) != SBI_HSM_STATE_STARTED)
                break;
            semu_process_hsm_resume_if_started(hart);

            emu_tick_peripherals(emu);
            emu_update_timer_interrupt(hart);
            emu_update_swi_interrupt(hart);
            if (unlikely(semu_step_chunk(emu, hart, SEMU_SMP_SLICE_STEPS))) {
                emu_threaded_fatal_store(emu, true);
                semu_set_stopped(emu, true);
                break;
            }
            if (semu_hart_pause_safe_point(hart) < 0)
                break;
            if (hart->error == ERR_USER) {
                hart->error = ERR_NONE;
                break;
            }
        }
    }

    return NULL;
}

static void io_poll_peripherals_threaded(emu_state_t *emu)
{
    io_poll_peripherals(emu);
    semu_wake_interruptible_harts(emu);
}

static void *io_thread_func(void *arg)
{
    emu_state_t *emu = (emu_state_t *) arg;

    while (!emu_stopped_load(emu)) {
        if (signal_received) {
            semu_set_stopped(emu, true);
            break;
        }

        io_poll_peripherals_threaded(emu);
        for (uint32_t i = 0; i < emu->vm.n_hart; i++) {
            emu_update_timer_interrupt(emu->vm.hart[i]);
            emu_update_swi_interrupt(emu->vm.hart[i]);
            semu_wake_hart_if_interrupt_pending(emu, i);
        }
        poll(NULL, 0, 1);
    }

    semu_signal_all_harts(emu);
    return NULL;
}

static int semu_step(emu_state_t *emu)
{
    vm_t *vm = &emu->vm;

    for (uint32_t i = 0; i < vm->n_hart; i++) {
        if (semu_service_hart_step(emu, vm->hart[i]))
            return 2;
    }

    return 0;
}

static int semu_service_hart_step(emu_state_t *emu, hart_t *hart)
{
    semu_process_pending_rfence(hart);
    emu_tick_peripherals(emu);
    emu_update_timer_interrupt(hart);
    emu_update_swi_interrupt(hart);
    return semu_step_chunk(emu, hart, 1);
}

static int semu_run_chunk(emu_state_t *emu, int steps)
{
    hart_t *hart = emu->vm.hart[0];

    emu_tick_peripherals(emu);
    emu_update_timer_interrupt(hart);
    emu_update_swi_interrupt(hart);
    return semu_step_chunk(emu, hart, steps);
}

static int semu_step_chunk(emu_state_t *emu, hart_t *hart, int steps)
{
    while (steps > 0) {
        int executed = vm_step_many(hart, steps);
        steps -= executed;
        if (likely(!hart->error))
            return 0;

        if (hart->error == ERR_EXCEPTION && hart->exc_cause == RV_EXC_ECALL_S) {
            handle_sbi_ecall(hart);
            if (hart->error == ERR_USER) {
                semu_finish_hsm_park(emu, hart);
                hart->error = ERR_NONE;
                return 0;
            }
            if (unlikely(emu_stopped_load(emu)))
                return 0;
            continue;
        }

        if (hart->error == ERR_USER) {
            semu_finish_hsm_park(emu, hart);
            hart->error = ERR_NONE;
            return 0;
        }

        if (hart->error == ERR_EXCEPTION) {
            hart_trap(hart);
            continue;
        }

        vm_error_report(hart);
        return 2;
    }

    return 0;
}

/* Async-signal-safe SIGINT/SIGTERM handler. Setting this flag lets the
 * event loops break out cleanly so atexit hooks (e.g., the virtio-blk
 * msync) actually run instead of being skipped by an immediate process
 * death. The MMU_CACHE_STATS build additionally uses the flag to defer
 * stats printing out of the signal handler.
 *
 * When VIRTIOINPUT runs the emulator in a background thread, the signal
 * is typically delivered to the main thread. The emu thread can be
 * blocked in poll(-1), so we also write a byte to its wake pipe; that
 * guarantees the loop notices `signal_received` and exits, which in
 * turn unblocks the window main loop via window_shutdown().
 */
static volatile sig_atomic_t signal_wake_fd = -1;
static void signal_handler(int sig UNUSED)
{
    signal_received = 1;
    int fd = signal_wake_fd;
    if (fd >= 0) {
        char byte = 1;
        /* write() is async-signal-safe; pipe is non-blocking so this
         * cannot stall the handler.
         */
        ssize_t n = write(fd, &byte, 1);
        (void) n;
    }
}

#if SEMU_HAS(VIRTIOINPUT) || SEMU_HAS(VIRTIOGPU)
static void semu_close_wake_pipe(emu_state_t *emu)
{
    signal_wake_fd = -1;
    if (g_window.window_set_wake_fd)
        g_window.window_set_wake_fd(-1);

    if (emu->wake_fd[0] >= 0) {
        close(emu->wake_fd[0]);
        emu->wake_fd[0] = -1;
    }
    if (emu->wake_fd[1] >= 0) {
        close(emu->wake_fd[1]);
        emu->wake_fd[1] = -1;
    }
}
#endif

#ifdef MMU_CACHE_STATS
static void print_mmu_cache_stats(vm_t *vm)
{
    fprintf(stderr, "\n=== MMU Cache Statistics ===\n");
    for (uint32_t i = 0; i < vm->n_hart; i++) {
        hart_t *hart = vm->hart[i];

        /* Combine 2-entry TLB statistics */
        uint64_t fetch_hits_tlb = 0, fetch_misses_tlb = 0;
        fetch_hits_tlb =
            hart->cache_fetch[0].tlb_hits + hart->cache_fetch[1].tlb_hits;
        fetch_misses_tlb =
            hart->cache_fetch[0].tlb_misses + hart->cache_fetch[1].tlb_misses;
        uint64_t tlb_total = fetch_hits_tlb + fetch_misses_tlb;

        /* Combine I-cache statistics */
        uint64_t fetch_hits_icache = 0, fetch_misses_icache = 0;
        fetch_hits_icache =
            hart->cache_fetch[0].icache_hits + hart->cache_fetch[1].icache_hits;
        fetch_misses_icache = hart->cache_fetch[0].icache_misses +
                              hart->cache_fetch[1].icache_misses;

        uint64_t access_total =
            hart->cache_fetch[0].total_fetch + hart->cache_fetch[1].total_fetch;

        /* Combine 8-set × 2-way load cache statistics */
        uint64_t load_hits = 0, load_misses = 0;
        for (int set = 0; set < 8; set++) {
            for (int way = 0; way < 2; way++) {
                load_hits += hart->cache_load[set].ways[way].hits;
                load_misses += hart->cache_load[set].ways[way].misses;
            }
        }
        uint64_t load_total = load_hits + load_misses;

        /* Combine 8-set × 2-way store cache statistics */
        uint64_t store_hits = 0, store_misses = 0;
        for (int set = 0; set < 8; set++) {
            for (int way = 0; way < 2; way++) {
                store_hits += hart->cache_store[set].ways[way].hits;
                store_misses += hart->cache_store[set].ways[way].misses;
            }
        }
        uint64_t store_total = store_hits + store_misses;

        fprintf(stderr, "\nHart %u:\n", i);
        fprintf(stderr, "\n=== Introduction Cache Statistics ===\n");
        fprintf(stderr, "  Total access:  %12llu\n", access_total);
        if (access_total > 0) {
            fprintf(stderr, "  Icache hits:   %12llu (%.2f%%)\n",
                    fetch_hits_icache,
                    (fetch_hits_icache * 100.0) / access_total);

            fprintf(stderr, "  Icache misses: %12llu (%.2f%%)\n",
                    fetch_misses_icache,
                    (fetch_misses_icache * 100.0) / access_total);
        }
        if (tlb_total > 0) {
            fprintf(stderr, "   ├ TLB hits:   %12llu (%.2f%%)\n",
                    fetch_hits_tlb, (fetch_hits_tlb * 100.0) / (tlb_total));
            fprintf(stderr, "   └ TLB misses: %12llu (%.2f%%)\n",
                    fetch_misses_tlb, (fetch_misses_tlb * 100.0) / (tlb_total));
        }
        fprintf(stderr, "\n=== Data Cache Statistics ===\n");
        fprintf(stderr, "  Load:  %12llu hits, %12llu misses (8x2)", load_hits,
                load_misses);
        if (load_total > 0)
            fprintf(stderr, " (%.2f%% hit rate)",
                    100.0 * load_hits / load_total);
        fprintf(stderr, "\n");

        fprintf(stderr, "  Store: %12llu hits, %12llu misses (8x2)", store_hits,
                store_misses);
        if (store_total > 0)
            fprintf(stderr, " (%.2f%% hit rate)",
                    100.0 * store_hits / store_total);
        fprintf(stderr, "\n");
    }
}
#endif


static void semu_run_threaded(emu_state_t *emu)
{
    vm_t *vm = &emu->vm;
    uint32_t created_harts = 0;

    if (pthread_create(&emu->io_thread, NULL, io_thread_func, emu) != 0) {
        fprintf(stderr, "Failed to create I/O thread\n");
        emu_threaded_fatal_store(emu, true);
        emu->exit_code = 1;
        return;
    }
    emu->io_thread_created = true;

    for (uint32_t i = 0; i < vm->n_hart; i++) {
        if (pthread_create(&emu->hart_threads[i], NULL, hart_thread_func,
                           vm->hart[i]) != 0) {
            fprintf(stderr, "Failed to create hart thread %u\n", i);
            emu_threaded_fatal_store(emu, true);
            semu_set_stopped(emu, true);
            break;
        }
        created_harts++;
    }

    while (!emu_stopped_load(emu)) {
        if (signal_received) {
            semu_set_stopped(emu, true);
            break;
        }
        poll(NULL, 0, 10);
    }

    semu_set_stopped(emu, true);
    for (uint32_t i = 0; i < created_harts; i++)
        pthread_join(emu->hart_threads[i], NULL);
    if (emu->io_thread_created)
        pthread_join(emu->io_thread, NULL);

    emu->exit_code = emu_threaded_fatal_load(emu) ? 1 : 0;
}

static void semu_run(emu_state_t *emu)
{
    if (semu_should_use_threaded_runtime(emu)) {
        semu_run_threaded(emu);
        return;
    }

    int ret;

    /* Single-hart mode: use inline scheduling. */
    while (!emu_stopped_load(emu)) {
        /* Break out on SIGINT/SIGTERM so atexit hooks fire on graceful exit. */
        if (signal_received)
            break;
#if SEMU_HAS(VIRTIONET)
        int i = 0;
        if (emu->vnet.peer.type == NETDEV_IMPL_user &&
            semu_boot_complete_load()) {
            net_user_options_t *usr = (net_user_options_t *) emu->vnet.peer.op;

            uint32_t timeout = -1;
            usr->pfd_len = 1;
            slirp_pollfds_fill_socket(usr->slirp, &timeout,
                                      semu_slirp_add_poll_socket, usr);

            /* Poll the internal pipe for incoming data. If data is
             * available (POLL_IN), process it and forward it to the
             * virtio-net device.
             */
            int pollout = poll(usr->pfd, usr->pfd_len, 1);
            if (usr->pfd[0].revents & POLLIN) {
                virtio_net_recv_from_peer(usr->peer);
            }
            slirp_pollfds_poll(usr->slirp, (pollout <= 0),
                               semu_slirp_get_revents, usr);
            for (i = 0; i < SLIRP_POLL_INTERVAL; i += SEMU_SLIRP_SLICE_STEPS) {
                int steps =
                    MIN(SEMU_SLIRP_SLICE_STEPS, SLIRP_POLL_INTERVAL - i);

                ret = semu_run_chunk(emu, steps);
                if (ret) {
                    emu->exit_code = ret;
                    return;
                }
            }
        } else
#endif
        {
            ret = semu_run_chunk(emu, SEMU_SINGLE_SLICE_STEPS);
            if (ret) {
                emu->exit_code = ret;
                return;
            }
        }
    }

    /* unreachable */
    emu->exit_code = 0;
}

static inline bool semu_is_interrupt(emu_state_t *emu)
{
    return __atomic_load_n(&emu->is_interrupted, __ATOMIC_RELAXED);
}

static bool semu_debug_config_supported(const emu_state_t *emu)
{
    return emu->vm.n_hart == 1;
}

static bool semu_debug_reject_unsupported_config(emu_state_t *emu)
{
    if (semu_debug_config_supported(emu))
        return false;

    fprintf(stderr,
            "SMP gdbstub/debug is unsupported until stop-all-harts debug "
            "semantics exist; use -c 1 with --gdbstub, or run SMP without "
            "--gdbstub.\n");
    emu->exit_code = 1;
    return true;
}

static bool semu_debug_cpu_id_valid(const emu_state_t *emu, int cpuid)
{
    return cpuid >= 0 && (uint32_t) cpuid < emu->vm.n_hart;
}

static size_t semu_get_reg_bytes(UNUSED int regno)
{
    return 4;
}

static int semu_read_reg(void *args, int regno, void *data)
{
    emu_state_t *emu = (emu_state_t *) args;

    if (regno < 0 || regno > 32)
        return EFAULT;

    if (!semu_debug_cpu_id_valid(emu, emu->curr_cpuid))
        return EFAULT;

    if (regno == 32)
        *(uint32_t *) data = emu->vm.hart[emu->curr_cpuid]->pc;
    else
        *(uint32_t *) data = emu->vm.hart[emu->curr_cpuid]->x_regs[regno];

    return 0;
}

static int semu_read_mem(void *args, size_t addr, size_t len, void *val)
{
    emu_state_t *emu = (emu_state_t *) args;

    if (!semu_debug_cpu_id_valid(emu, emu->curr_cpuid))
        return EFAULT;

    hart_t *hart = emu->vm.hart[emu->curr_cpuid];
    mem_load(hart, addr, len, val);
    return 0;
}

static gdb_action_t semu_cont(void *args)
{
    emu_state_t *emu = (emu_state_t *) args;

    /* A previous terminal interrupt should stop only the active continue.
     * Clear the sticky global flag before resuming so later GDB `continue`
     * commands can run guest code again.
     */
    signal_received = 0;
#if SEMU_HAS(VIRTIOINPUT) || SEMU_HAS(VIRTIOGPU)
    while (!semu_is_interrupt(emu) && !g_window.window_is_closed()) {
#else
    while (!semu_is_interrupt(emu)) {
#endif
        /* Break out on SIGINT/SIGTERM so the gdbstub regains control
         * and main() returns through the atexit hooks.
         */
        if (signal_received)
            break;
        semu_step(emu);
    }

    /* Clear the interrupt if it's pending */
    __atomic_store_n(&emu->is_interrupted, false, __ATOMIC_RELAXED);

#if SEMU_HAS(VIRTIOINPUT) || SEMU_HAS(VIRTIOGPU)
    /* Tell gdbstub_run() to exit cleanly when the window is closed. */
    if (g_window.window_is_closed())
        return ACT_SHUTDOWN;
#endif
    return ACT_RESUME;
}

static gdb_action_t semu_stepi(void *args)
{
    emu_state_t *emu = (emu_state_t *) args;
    semu_step(emu);
    return ACT_RESUME;
}

static void semu_on_interrupt(void *args)
{
    emu_state_t *emu = (emu_state_t *) args;
    /* Notify the emulator to break out the for loop in rv_cont */
    __atomic_store_n(&emu->is_interrupted, true, __ATOMIC_RELAXED);
}

static int semu_get_cpu(void *args)
{
    emu_state_t *emu = (emu_state_t *) args;
    return emu->curr_cpuid;
}

static void semu_set_cpu(void *args, int cpuid)
{
    emu_state_t *emu = (emu_state_t *) args;

    if (!semu_debug_cpu_id_valid(emu, cpuid))
        return;

    emu->curr_cpuid = cpuid;
}

static void semu_run_debug(emu_state_t *emu)
{
    if (semu_debug_reject_unsupported_config(emu))
        return;

    vm_t *vm = &emu->vm;

    gdbstub_t gdbstub;
    struct target_ops gdbstub_ops = {
        .get_reg_bytes = semu_get_reg_bytes,
        .read_reg = semu_read_reg,
        .write_reg = NULL,
        .read_mem = semu_read_mem,
        .write_mem = NULL,
        .cont = semu_cont,
        .stepi = semu_stepi,
        .set_bp = NULL,
        .del_bp = NULL,
        .on_interrupt = semu_on_interrupt,

        .get_cpu = semu_get_cpu,
        .set_cpu = semu_set_cpu,
    };

    emu->curr_cpuid = 0;
    if (!gdbstub_init(&gdbstub, &gdbstub_ops,
                      (arch_info_t) {
                          .smp = vm->n_hart,
                          .reg_num = 33,
                          .target_desc = TARGET_RV32,
                      },
                      "127.0.0.1:1234")) {
        emu->exit_code = 1;
        return;
    }

    __atomic_store_n(&emu->is_interrupted, false, __ATOMIC_RELAXED);
    bool ok = gdbstub_run(&gdbstub, (void *) emu);

    gdbstub_close(&gdbstub);

    emu->exit_code = ok ? 0 : 1;
}

#if SEMU_HAS(VIRTIOINPUT) || SEMU_HAS(VIRTIOGPU)
/* Thread wrapper for backends that reserve the main thread for
 * 'window_main_loop()'.
 */
static void *emu_thread_func(void *arg)
{
    emu_state_t *emu = (emu_state_t *) arg;

    if (emu->debug)
        semu_run_debug(emu);
    else
        semu_run(emu);

    /* Unblock 'window_main_loop()' on the main thread so it can return. */
    if (g_window.window_shutdown)
        g_window.window_shutdown();

    return NULL;
}
#endif

int main(int argc, char **argv)
{
    int ret;
    emu_state_t emu;
    ret = semu_init(&emu, argc, argv);
    if (ret)
        return ret;

    /* Install handlers unconditionally so SIGINT/SIGTERM let us return
     * through main() and run atexit hooks (msync of MAP_SHARED disks,
     * etc.) instead of being killed mid-flight. Use sigaction so the
     * SA_RESTART flag stays clear -- otherwise glibc's `signal()` would
     * auto-restart `poll()` and the loops would never see the flag.
     */
    /* Use sigaction so the SA_RESTART flag stays clear -- otherwise
     * glibc's `signal()` would auto-restart `poll()` and the loops
     * would never see the flag.
     */
    {
        struct sigaction sa = {0};
        sa.sa_handler = signal_handler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;
        sigaction(SIGINT, &sa, NULL);
        sigaction(SIGTERM, &sa, NULL);
    }

#if SEMU_HAS(VIRTIOINPUT) || SEMU_HAS(VIRTIOGPU)
    /* Publish the wake pipe to the signal handler so SIGINT/SIGTERM can
     * unblock the emulator thread's poll() in the threaded window path.
     */
    if (emu.wake_fd[1] >= 0)
        signal_wake_fd = emu.wake_fd[1];

    /* If the window backend provides 'window_main_loop()', run the emulator in
     * a background thread and use the main thread for window events.
     */
    if (g_window.window_main_loop) {
        pthread_t emu_thread;

        if (emu.wake_fd[1] >= 0)
            g_window.window_set_wake_fd(emu.wake_fd[1]);

        if (pthread_create(&emu_thread, NULL, emu_thread_func, &emu) != 0) {
            fprintf(stderr, "Failed to create emulator thread\n");
            semu_close_wake_pipe(&emu);
#if SEMU_HAS(VIRTIOGPU)
            virtio_gpu_destroy(&emu.vgpu);
            vgpu_display_shutdown_after_producer_stopped();
#endif
            g_window.window_cleanup();
            return 1;
        }

        /* Main thread runs window event loop. Returns either because the user
         * closed the window ('SDL_QUIT') or because the emulator called
         * 'window_shutdown()'. 'emu_tick_peripherals()' picks up the window
         * backend's closed state and sets 'emu->stopped', so no direct write to
         * 'emu.stopped' is needed here.
         */
        g_window.window_main_loop();

        /* Wait for emulator thread to finish. */
        pthread_join(emu_thread, NULL);
    } else
#endif
    {
        if (emu.debug)
            semu_run_debug(&emu);
        else
            semu_run(&emu);
    }

#if SEMU_HAS(VIRTIOINPUT) || SEMU_HAS(VIRTIOGPU)
    semu_close_wake_pipe(&emu);
#if SEMU_HAS(VIRTIOGPU)
    /* The GPU actor/backend may publish display commands; stop it before
     * queued payloads are drained and SDL state is cleaned up.
     */
    virtio_gpu_destroy(&emu.vgpu);
    vgpu_display_shutdown_after_producer_stopped();
#endif
    g_window.window_cleanup();
#endif

#ifdef MMU_CACHE_STATS
    print_mmu_cache_stats(&emu.vm);
#endif
    return emu.exit_code;
}
