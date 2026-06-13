#pragma once

#include <stdbool.h>
#include <stdint.h>

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
