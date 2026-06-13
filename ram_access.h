#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "guest-types.h"

typedef struct __vm_internel vm_t;

typedef void (*ram_dma_write_invalidate_cb_t)(void *opaque,
                                              guest_paddr_t addr,
                                              guest_size_t len);

typedef struct ram_dma {
    uint32_t *words;
    guest_size_t byte_size;
    vm_t *machine;
    _Atomic uint64_t dirty_bytes;
    _Atomic guest_paddr_t dirty_start;
    _Atomic guest_paddr_t dirty_end;
    ram_dma_write_invalidate_cb_t write_invalidate_cb;
    void *write_invalidate_opaque;
} ram_dma_t;

void ram_dma_init(ram_dma_t *dma,
                  uint32_t *words,
                  guest_size_t byte_size,
                  vm_t *machine);
void ram_dma_set_write_invalidate_callback(ram_dma_t *dma,
                                           ram_dma_write_invalidate_cb_t cb,
                                           void *opaque);
bool ram_dma_read(const ram_dma_t *dma,
                  guest_paddr_t addr,
                  void *dst,
                  guest_size_t len);
bool ram_dma_write(ram_dma_t *dma,
                   guest_paddr_t addr,
                   const void *src,
                   guest_size_t len);
void ram_note_dma_write(ram_dma_t *dma, guest_paddr_t addr, guest_size_t len);
uint64_t ram_dma_dirty_bytes(const ram_dma_t *dma);
bool ram_dma_dirty_range(const ram_dma_t *dma,
                         guest_paddr_t *start,
                         guest_paddr_t *end);
void ram_dma_clear_dirty(ram_dma_t *dma);

static inline uint32_t ram_load_w(const uint32_t *cell)
{
    return __atomic_load_n(cell, __ATOMIC_RELAXED);
}

static inline uint32_t ram_load_w_acquire(const uint32_t *cell)
{
    return __atomic_load_n(cell, __ATOMIC_ACQUIRE);
}

static inline void ram_store_w(uint32_t *cell, uint32_t value)
{
    __atomic_store_n(cell, value, __ATOMIC_RELAXED);
}

static inline void ram_store_w_release(uint32_t *cell, uint32_t value)
{
    __atomic_store_n(cell, value, __ATOMIC_RELEASE);
}

static inline void ram_store_subword(uint32_t *cell,
                                     uint32_t mask,
                                     uint32_t bits)
{
    uint32_t old = ram_load_w(cell);

    while (!__atomic_compare_exchange_n(cell, &old, (old & ~mask) | bits, true,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED))
        ;
}

static inline void ram_fetch_or_w(uint32_t *cell, uint32_t bits)
{
    __atomic_fetch_or(cell, bits, __ATOMIC_RELAXED);
}

static inline uint16_t ram_load_high16_acquire(const uint32_t *cell)
{
    return (uint16_t) (ram_load_w_acquire(cell) >> 16);
}

static inline uint16_t ram_load_high16(const uint32_t *cell)
{
    return (uint16_t) (ram_load_w(cell) >> 16);
}

static inline void ram_store_high16_release(uint32_t *cell, uint16_t value)
{
    uint32_t low = ram_load_w(cell) & 0xffffU;
    ram_store_w_release(cell, low | ((uint32_t) value << 16));
}
