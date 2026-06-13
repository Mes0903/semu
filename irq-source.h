#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "riscv.h"

struct emu_state;
struct plic_state;

typedef struct emu_state emu_state_t;
typedef struct plic_state plic_state_t;

enum semu_irq_source {
    SEMU_IRQ_SOURCE_UART,
    SEMU_IRQ_SOURCE_VNET,
    SEMU_IRQ_SOURCE_VBLK,
    SEMU_IRQ_SOURCE_VRNG,
    SEMU_IRQ_SOURCE_VSND,
    SEMU_IRQ_SOURCE_VFS,
    SEMU_IRQ_SOURCE_VINPUT_KEYBOARD,
    SEMU_IRQ_SOURCE_VINPUT_MOUSE,
    SEMU_IRQ_SOURCE_VGPU,
    SEMU_IRQ_SOURCE_COUNT,
};

uint32_t semu_irq_source_plic_id(enum semu_irq_source source);
uint32_t semu_irq_source_plic_bit(enum semu_irq_source source);
bool semu_irq_source_apply_level(vm_t *vm,
                                 plic_state_t *plic,
                                 enum semu_irq_source source,
                                 bool level);
void semu_irq_source_set(emu_state_t *emu,
                         enum semu_irq_source source,
                         bool level);
void semu_irq_source_pulse(emu_state_t *emu, enum semu_irq_source source);

void semu_wake_interruptible_harts(emu_state_t *emu);
