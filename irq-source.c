#include "irq-source.h"

#include <pthread.h>

#include "device.h"
#include "platform.h"

uint32_t semu_irq_source_plic_id(enum semu_irq_source source)
{
    switch (source) {
    case SEMU_IRQ_SOURCE_UART:
        return SEMU_PLATFORM_IRQ_UART;
    case SEMU_IRQ_SOURCE_VNET:
        return SEMU_PLATFORM_IRQ_VNET;
    case SEMU_IRQ_SOURCE_VBLK:
        return SEMU_PLATFORM_IRQ_VBLK;
    case SEMU_IRQ_SOURCE_VRNG:
        return SEMU_PLATFORM_IRQ_VRNG;
    case SEMU_IRQ_SOURCE_VSND:
        return SEMU_PLATFORM_IRQ_VSND;
    case SEMU_IRQ_SOURCE_VFS:
        return SEMU_PLATFORM_IRQ_VFS;
    case SEMU_IRQ_SOURCE_VINPUT_KEYBOARD:
        return SEMU_PLATFORM_IRQ_VINPUT_KEYBOARD;
    case SEMU_IRQ_SOURCE_VINPUT_MOUSE:
        return SEMU_PLATFORM_IRQ_VINPUT_MOUSE;
    case SEMU_IRQ_SOURCE_VGPU:
        return SEMU_PLATFORM_IRQ_VGPU;
    case SEMU_IRQ_SOURCE_COUNT:
    default:
        return SEMU_PLATFORM_IRQ_NONE;
    }
}

uint32_t semu_irq_source_plic_bit(enum semu_irq_source source)
{
    uint32_t id = semu_irq_source_plic_id(source);

    if (id == SEMU_PLATFORM_IRQ_NONE || id >= 32)
        return 0;
    return UINT32_C(1) << id;
}

bool semu_irq_source_apply_level(vm_t *vm,
                                 plic_state_t *plic,
                                 enum semu_irq_source source,
                                 bool level)
{
    uint32_t id = semu_irq_source_plic_id(source);

    if (id == SEMU_PLATFORM_IRQ_NONE)
        return false;
    return plic_set_source_level(vm, plic, id, level);
}

void semu_irq_source_set(emu_state_t *emu,
                         enum semu_irq_source source,
                         bool level)
{
    bool valid;

    if (!emu)
        return;

    pthread_mutex_lock(&emu->plic_lock);
    valid = semu_irq_source_apply_level(&emu->vm, &emu->plic, source, level);
    pthread_mutex_unlock(&emu->plic_lock);

    if (valid)
        semu_wake_interruptible_harts(emu);
}

void semu_irq_source_pulse(emu_state_t *emu, enum semu_irq_source source)
{
    semu_irq_source_set(emu, source, true);
    semu_irq_source_set(emu, source, false);
}
