#include <stddef.h>
#include <string.h>

#include "mmio-bus.h"

static bool semu_mmio_region_end(const struct semu_mmio_region *region,
                                 uint64_t *end)
{
    if (!region || region->size == 0)
        return false;
    if (region->base > UINT64_MAX - region->size)
        return false;

    *end = region->base + region->size;
    return true;
}

static bool semu_mmio_regions_overlap(const struct semu_mmio_region *left,
                                      const struct semu_mmio_region *right)
{
    uint64_t left_end;
    uint64_t right_end;

    if (!semu_mmio_region_end(left, &left_end) ||
        !semu_mmio_region_end(right, &right_end))
        return true;

    return left->base < right_end && right->base < left_end;
}

void semu_mmio_bus_init(struct semu_mmio_bus *bus)
{
    if (!bus)
        return;

    memset(bus, 0, sizeof(*bus));
}

bool semu_mmio_bus_register(struct semu_mmio_bus *bus,
                            const struct semu_mmio_region *region)
{
    uint64_t end;

    if (!bus || !region)
        return false;
    if (!semu_mmio_region_end(region, &end))
        return false;
    if (bus->count >= SEMU_MMIO_BUS_MAX_REGIONS)
        return false;

    for (uint32_t i = 0; i < bus->count; i++) {
        if (semu_mmio_regions_overlap(&bus->regions[i], region))
            return false;
    }

    bus->regions[bus->count++] = *region;
    bus->generation++;
    return true;
}

const struct semu_mmio_region *semu_mmio_bus_find(
    const struct semu_mmio_bus *bus,
    uint64_t addr,
    uint64_t *off)
{
    if (!bus)
        return NULL;

    for (uint32_t i = 0; i < bus->count; i++) {
        const struct semu_mmio_region *region = &bus->regions[i];
        uint64_t end;

        if (!semu_mmio_region_end(region, &end))
            continue;
        if (addr >= region->base && addr < end) {
            if (off)
                *off = addr - region->base;
            return region;
        }
    }

    return NULL;
}

bool semu_mmio_bus_read(struct semu_mmio_bus *bus,
                        hart_t *hart,
                        uint64_t addr,
                        uint8_t width,
                        uint32_t *value)
{
    uint64_t off;
    const struct semu_mmio_region *region = semu_mmio_bus_find(bus, addr, &off);

    if (!region || !region->read)
        return false;

    return region->read(hart, region->opaque, off, width, value);
}

bool semu_mmio_bus_write(struct semu_mmio_bus *bus,
                         hart_t *hart,
                         uint64_t addr,
                         uint8_t width,
                         uint32_t value)
{
    uint64_t off;
    const struct semu_mmio_region *region = semu_mmio_bus_find(bus, addr, &off);

    if (!region || !region->write)
        return false;

    return region->write(hart, region->opaque, off, width, value);
}
