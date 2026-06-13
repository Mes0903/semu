#include "platform.h"

#include "mmio-bus.h"

#define PLATFORM_ARRAY_SIZE(a) (sizeof(a) / sizeof(*(a)))

static const struct semu_platform_device fixed_devices[] = {
    {
        .name = "plic-window-0",
        .base = SEMU_PLATFORM_MMIO_PLIC_WINDOW0_BASE,
        .size = SEMU_PLATFORM_MMIO_REGION_SIZE,
        .irq_source = SEMU_PLATFORM_IRQ_NONE,
    },
    {
        .name = "plic-window-2",
        .base = SEMU_PLATFORM_MMIO_PLIC_WINDOW2_BASE,
        .size = SEMU_PLATFORM_MMIO_REGION_SIZE,
        .irq_source = SEMU_PLATFORM_IRQ_NONE,
    },
    {
        .name = "uart",
        .base = SEMU_PLATFORM_MMIO_UART_BASE,
        .size = SEMU_PLATFORM_MMIO_REGION_SIZE,
        .irq_source = SEMU_PLATFORM_IRQ_UART,
    },
    {
        .name = "virtio-net",
        .base = SEMU_PLATFORM_MMIO_VNET_BASE,
        .size = SEMU_PLATFORM_MMIO_REGION_SIZE,
        .irq_source = SEMU_PLATFORM_IRQ_VNET,
    },
    {
        .name = "virtio-blk",
        .base = SEMU_PLATFORM_MMIO_VBLK_BASE,
        .size = SEMU_PLATFORM_MMIO_REGION_SIZE,
        .irq_source = SEMU_PLATFORM_IRQ_VBLK,
    },
    {
        .name = "mtimer",
        .base = SEMU_PLATFORM_MMIO_MTIMER_BASE,
        .size = SEMU_PLATFORM_MMIO_REGION_SIZE,
        .irq_source = SEMU_PLATFORM_IRQ_NONE,
    },
    {
        .name = "mswi",
        .base = SEMU_PLATFORM_MMIO_MSWI_BASE,
        .size = SEMU_PLATFORM_MMIO_REGION_SIZE,
        .irq_source = SEMU_PLATFORM_IRQ_NONE,
    },
    {
        .name = "sswi",
        .base = SEMU_PLATFORM_MMIO_SSWI_BASE,
        .size = SEMU_PLATFORM_MMIO_REGION_SIZE,
        .irq_source = SEMU_PLATFORM_IRQ_NONE,
    },
    {
        .name = "virtio-rng",
        .base = SEMU_PLATFORM_MMIO_VRNG_BASE,
        .size = SEMU_PLATFORM_MMIO_REGION_SIZE,
        .irq_source = SEMU_PLATFORM_IRQ_VRNG,
    },
    {
        .name = "virtio-snd",
        .base = SEMU_PLATFORM_MMIO_VSND_BASE,
        .size = SEMU_PLATFORM_MMIO_REGION_SIZE,
        .irq_source = SEMU_PLATFORM_IRQ_VSND,
    },
    {
        .name = "virtio-fs",
        .base = SEMU_PLATFORM_MMIO_VFS_BASE,
        .size = SEMU_PLATFORM_MMIO_REGION_SIZE,
        .irq_source = SEMU_PLATFORM_IRQ_VFS,
    },
    {
        .name = "virtio-input-keyboard",
        .base = SEMU_PLATFORM_MMIO_VINPUT_KEYBOARD_BASE,
        .size = SEMU_PLATFORM_MMIO_REGION_SIZE,
        .irq_source = SEMU_PLATFORM_IRQ_VINPUT_KEYBOARD,
    },
    {
        .name = "virtio-input-mouse",
        .base = SEMU_PLATFORM_MMIO_VINPUT_MOUSE_BASE,
        .size = SEMU_PLATFORM_MMIO_REGION_SIZE,
        .irq_source = SEMU_PLATFORM_IRQ_VINPUT_MOUSE,
    },
    {
        .name = "virtio-gpu",
        .base = SEMU_PLATFORM_MMIO_VGPU_BASE,
        .size = SEMU_PLATFORM_MMIO_REGION_SIZE,
        .irq_source = SEMU_PLATFORM_IRQ_VGPU,
    },
};

_Static_assert(PLATFORM_ARRAY_SIZE(fixed_devices) == SEMU_PLATFORM_DEVICE_COUNT,
               "platform device count must match fixed table");

const struct semu_platform_device *semu_platform_devices(size_t *count)
{
    if (count)
        *count = PLATFORM_ARRAY_SIZE(fixed_devices);

    return fixed_devices;
}

bool semu_platform_register_fixed_mmio_configured(
    struct semu_mmio_bus *bus,
    semu_platform_mmio_configure_fn configure,
    void *opaque)
{
    for (size_t i = 0; i < PLATFORM_ARRAY_SIZE(fixed_devices); i++) {
        const struct semu_platform_device *device = &fixed_devices[i];
        struct semu_mmio_region region = {
            .base = device->base,
            .size = device->size,
            .name = device->name,
            .irq_source = device->irq_source,
        };

        if (configure)
            configure(device, &region, opaque);

        if (!semu_mmio_bus_register(bus, &region))
            return false;
    }

    return true;
}

bool semu_platform_register_fixed_mmio(struct semu_mmio_bus *bus)
{
    return semu_platform_register_fixed_mmio_configured(bus, NULL, NULL);
}
