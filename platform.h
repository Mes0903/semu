#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct semu_mmio_bus;
struct semu_mmio_region;

/*
 * irq_source stores the PLIC source ID for devices that raise an external
 * interrupt. SEMU_PLATFORM_IRQ_NONE means the window has no external PLIC
 * source; PLIC and ACLINT windows use that sentinel.
 */
#define SEMU_PLATFORM_IRQ_NONE UINT32_MAX
#define SEMU_PLATFORM_IRQ_UART UINT32_C(1)
#define SEMU_PLATFORM_IRQ_VNET UINT32_C(2)
#define SEMU_PLATFORM_IRQ_VBLK UINT32_C(3)
#define SEMU_PLATFORM_IRQ_VRNG UINT32_C(4)
#define SEMU_PLATFORM_IRQ_VSND UINT32_C(5)
#define SEMU_PLATFORM_IRQ_VFS UINT32_C(6)
#define SEMU_PLATFORM_IRQ_VINPUT_KEYBOARD UINT32_C(7)
#define SEMU_PLATFORM_IRQ_VINPUT_MOUSE UINT32_C(8)
#define SEMU_PLATFORM_IRQ_VGPU UINT32_C(9)

#define SEMU_PLATFORM_MMIO_BASE UINT64_C(0xF0000000)
#define SEMU_PLATFORM_MMIO_REGION_SIZE UINT64_C(0x00100000)

#define SEMU_PLATFORM_MMIO_PLIC_WINDOW0_BASE UINT64_C(0xF0000000)
#define SEMU_PLATFORM_MMIO_PLIC_WINDOW2_BASE UINT64_C(0xF0200000)
#define SEMU_PLATFORM_MMIO_UART_BASE UINT64_C(0xF4000000)
#define SEMU_PLATFORM_MMIO_VNET_BASE UINT64_C(0xF4100000)
#define SEMU_PLATFORM_MMIO_VBLK_BASE UINT64_C(0xF4200000)
#define SEMU_PLATFORM_MMIO_MTIMER_BASE UINT64_C(0xF4300000)
#define SEMU_PLATFORM_MMIO_MSWI_BASE UINT64_C(0xF4400000)
#define SEMU_PLATFORM_MMIO_SSWI_BASE UINT64_C(0xF4500000)
#define SEMU_PLATFORM_MMIO_VRNG_BASE UINT64_C(0xF4600000)
#define SEMU_PLATFORM_MMIO_VSND_BASE UINT64_C(0xF4700000)
#define SEMU_PLATFORM_MMIO_VFS_BASE UINT64_C(0xF4800000)
#define SEMU_PLATFORM_MMIO_VINPUT_KEYBOARD_BASE UINT64_C(0xF4900000)
#define SEMU_PLATFORM_MMIO_VINPUT_MOUSE_BASE UINT64_C(0xF4A00000)
#define SEMU_PLATFORM_MMIO_VGPU_BASE UINT64_C(0xF4B00000)

#define SEMU_PLATFORM_DEVICE_COUNT 14U

struct semu_platform_device {
    const char *name;
    uint64_t base;
    uint64_t size;
    uint32_t irq_source;
};

typedef void (*semu_platform_mmio_configure_fn)(
    const struct semu_platform_device *device,
    struct semu_mmio_region *region,
    void *opaque);

const struct semu_platform_device *semu_platform_devices(size_t *count);
bool semu_platform_register_fixed_mmio_configured(
    struct semu_mmio_bus *bus,
    semu_platform_mmio_configure_fn configure,
    void *opaque);
bool semu_platform_register_fixed_mmio(struct semu_mmio_bus *bus);
