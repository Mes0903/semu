#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct __hart_internal hart_t;

#define SEMU_MMIO_BUS_MAX_REGIONS 32U

struct semu_mmio_region {
    uint64_t base;
    uint64_t size;
    const char *name;
    uint32_t irq_source;
    void *opaque;
    bool (*read)(hart_t *hart,
                 void *opaque,
                 uint64_t off,
                 uint8_t width,
                 uint32_t *value);
    bool (*write)(hart_t *hart,
                  void *opaque,
                  uint64_t off,
                  uint8_t width,
                  uint32_t value);
};

struct semu_mmio_bus {
    struct semu_mmio_region regions[SEMU_MMIO_BUS_MAX_REGIONS];
    uint32_t count;
    uint64_t generation;
};

void semu_mmio_bus_init(struct semu_mmio_bus *bus);
bool semu_mmio_bus_register(struct semu_mmio_bus *bus,
                            const struct semu_mmio_region *region);
const struct semu_mmio_region *semu_mmio_bus_find(
    const struct semu_mmio_bus *bus,
    uint64_t addr,
    uint64_t *off);
bool semu_mmio_bus_read(struct semu_mmio_bus *bus,
                        hart_t *hart,
                        uint64_t addr,
                        uint8_t width,
                        uint32_t *value);
bool semu_mmio_bus_write(struct semu_mmio_bus *bus,
                         hart_t *hart,
                         uint64_t addr,
                         uint8_t width,
                         uint32_t value);
