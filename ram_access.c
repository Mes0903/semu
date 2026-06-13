#include "ram_access.h"
#include "riscv.h"

#include <stddef.h>

#define RAM_DMA_NO_DIRTY_START ((guest_paddr_t) UINT64_MAX)

static bool ram_dma_bounds_ok(const ram_dma_t *dma,
                              guest_paddr_t addr,
                              guest_size_t len)
{
    if (!dma || !dma->words)
        return false;
    if (len == 0)
        return addr <= dma->byte_size;
    if (addr >= dma->byte_size)
        return false;
    return len <= dma->byte_size - addr;
}

static void ram_dma_atomic_min_guest(_Atomic guest_paddr_t *slot,
                                     guest_paddr_t value)
{
    guest_paddr_t old = __atomic_load_n(slot, __ATOMIC_RELAXED);

    while (value < old &&
           !__atomic_compare_exchange_n(slot, &old, value, true,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED))
        ;
}

static void ram_dma_atomic_max_guest(_Atomic guest_paddr_t *slot,
                                     guest_paddr_t value)
{
    guest_paddr_t old = __atomic_load_n(slot, __ATOMIC_RELAXED);

    while (value > old &&
           !__atomic_compare_exchange_n(slot, &old, value, true,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED))
        ;
}

static void ram_dma_mark_dirty(ram_dma_t *dma,
                               guest_paddr_t addr,
                               guest_size_t len)
{
    guest_paddr_t end;

    if (!dma || len == 0)
        return;

    if (len > UINT64_MAX - addr)
        end = UINT64_MAX;
    else
        end = addr + len;

    ram_dma_atomic_min_guest(&dma->dirty_start, addr);
    ram_dma_atomic_max_guest(&dma->dirty_end, end);
    __atomic_fetch_add(&dma->dirty_bytes, len, __ATOMIC_RELEASE);
}

static void ram_dma_notify_write_invalidate(ram_dma_t *dma,
                                            guest_paddr_t addr,
                                            guest_size_t len)
{
    ram_dma_write_invalidate_cb_t cb;

    if (!dma || len == 0)
        return;

    cb = dma->write_invalidate_cb;
    if (cb)
        cb(dma->write_invalidate_opaque, addr, len);
}

static void ram_dma_recompute_any_reservation_locked(vm_t *machine)
{
    bool any_active = false;

    if (!machine->reservations) {
        __atomic_store_n(&machine->any_reservation_active, false,
                         __ATOMIC_RELAXED);
        return;
    }

    for (uint32_t i = 0; i < machine->n_hart; i++)
        any_active |= machine->reservations[i].valid;

    __atomic_store_n(&machine->any_reservation_active, any_active,
                     __ATOMIC_RELAXED);
}

static void ram_dma_invalidate_reservations_locked(vm_t *machine,
                                                   guest_paddr_t addr,
                                                   guest_size_t len)
{
    guest_paddr_t first_word;
    guest_paddr_t last_word;

    if (!machine || !machine->reservations || len == 0)
        return;

    first_word = addr & ~(guest_paddr_t) 3;
    if (len > UINT64_MAX - addr)
        last_word = UINT64_MAX & ~(guest_paddr_t) 3;
    else
        last_word = (addr + len - 1) & ~(guest_paddr_t) 3;

    for (uint32_t i = 0; i < machine->n_hart; i++) {
        reservation_entry_t *entry = &machine->reservations[i];
        guest_paddr_t reservation_addr = entry->addr;

        if (entry->valid && reservation_addr >= first_word &&
            reservation_addr <= last_word)
            entry->valid = false;
    }

    ram_dma_recompute_any_reservation_locked(machine);
}

static void ram_dma_note_write_locked(ram_dma_t *dma,
                                      guest_paddr_t addr,
                                      guest_size_t len)
{
    if (!dma || len == 0)
        return;

    ram_dma_invalidate_reservations_locked(dma->machine, addr, len);
    ram_dma_mark_dirty(dma, addr, len);
}

void ram_dma_init(ram_dma_t *dma,
                  uint32_t *words,
                  guest_size_t byte_size,
                  vm_t *machine)
{
    dma->words = words;
    dma->byte_size = byte_size;
    dma->machine = machine;
    __atomic_store_n(&dma->dirty_bytes, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&dma->dirty_start, RAM_DMA_NO_DIRTY_START,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&dma->dirty_end, 0, __ATOMIC_RELAXED);
    dma->write_invalidate_cb = NULL;
    dma->write_invalidate_opaque = NULL;
}

void ram_dma_set_write_invalidate_callback(ram_dma_t *dma,
                                           ram_dma_write_invalidate_cb_t cb,
                                           void *opaque)
{
    if (!dma)
        return;

    dma->write_invalidate_cb = cb;
    dma->write_invalidate_opaque = opaque;
}

bool ram_dma_read(const ram_dma_t *dma,
                  guest_paddr_t addr,
                  void *dst,
                  guest_size_t len)
{
    uint8_t *out = dst;

    if (!ram_dma_bounds_ok(dma, addr, len) || (len != 0 && !dst))
        return false;

    for (guest_size_t i = 0; i < len; i++) {
        guest_paddr_t pos = addr + i;
        uint32_t word = ram_load_w(&dma->words[pos >> 2]);
        uint32_t shift = (uint32_t) ((pos & 3) * 8);

        out[i] = (uint8_t) (word >> shift);
    }

    return true;
}

bool ram_dma_write(ram_dma_t *dma,
                   guest_paddr_t addr,
                   const void *src,
                   guest_size_t len)
{
    const uint8_t *in = src;
    bool lock_reservations;

    if (!ram_dma_bounds_ok(dma, addr, len) || (len != 0 && !src))
        return false;

    if (len == 0)
        return true;

    lock_reservations = dma->machine != NULL;
    if (lock_reservations)
        pthread_mutex_lock(&dma->machine->reservation_lock);

    for (guest_size_t i = 0; i < len; i++) {
        guest_paddr_t pos = addr + i;
        uint32_t shift = (uint32_t) ((pos & 3) * 8);
        uint32_t mask = UINT32_C(0xff) << shift;
        uint32_t bits = (uint32_t) in[i] << shift;

        ram_store_subword(&dma->words[pos >> 2], mask, bits);
    }

    ram_dma_note_write_locked(dma, addr, len);

    if (lock_reservations)
        pthread_mutex_unlock(&dma->machine->reservation_lock);

    ram_dma_notify_write_invalidate(dma, addr, len);
    return true;
}

void ram_note_dma_write(ram_dma_t *dma, guest_paddr_t addr, guest_size_t len)
{
    if (!ram_dma_bounds_ok(dma, addr, len) || len == 0)
        return;

    if (dma->machine)
        pthread_mutex_lock(&dma->machine->reservation_lock);

    ram_dma_note_write_locked(dma, addr, len);

    if (dma->machine)
        pthread_mutex_unlock(&dma->machine->reservation_lock);

    ram_dma_notify_write_invalidate(dma, addr, len);
}

uint64_t ram_dma_dirty_bytes(const ram_dma_t *dma)
{
    if (!dma)
        return 0;
    return __atomic_load_n(&dma->dirty_bytes, __ATOMIC_RELAXED);
}

bool ram_dma_dirty_range(const ram_dma_t *dma,
                         guest_paddr_t *start,
                         guest_paddr_t *end)
{
    guest_paddr_t dirty_start;
    guest_paddr_t dirty_end;

    if (!dma || __atomic_load_n(&dma->dirty_bytes, __ATOMIC_ACQUIRE) == 0)
        return false;

    dirty_start = __atomic_load_n(&dma->dirty_start, __ATOMIC_RELAXED);
    dirty_end = __atomic_load_n(&dma->dirty_end, __ATOMIC_RELAXED);
    if (dirty_start == RAM_DMA_NO_DIRTY_START || dirty_end <= dirty_start)
        return false;

    if (start)
        *start = dirty_start;
    if (end)
        *end = dirty_end;
    return true;
}

void ram_dma_clear_dirty(ram_dma_t *dma)
{
    if (!dma)
        return;

    __atomic_store_n(&dma->dirty_bytes, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&dma->dirty_start, RAM_DMA_NO_DIRTY_START,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&dma->dirty_end, 0, __ATOMIC_RELAXED);
}
