#include "device.h"
#include "riscv.h"
#include "riscv_private.h"

/* Make PLIC as simple as possible: 32 interrupts, no priority */

enum {
    PLIC_ENABLE_BASE = 0x800,
    PLIC_ENABLE_STRIDE = 0x20,
    PLIC_CONTEXT_BASE = 0x80000,
    PLIC_CONTEXT_STRIDE = 0x400,
    PLIC_CONTEXT_THRESHOLD = 0,
    PLIC_CONTEXT_CLAIM = 1,
};

static bool plic_decode_enable_context(uint32_t addr, uint32_t *context)
{
    if (addr < PLIC_ENABLE_BASE)
        return false;

    uint32_t offset = addr - PLIC_ENABLE_BASE;
    if (offset % PLIC_ENABLE_STRIDE)
        return false;

    uint32_t decoded = offset / PLIC_ENABLE_STRIDE;
    if (decoded >= 32)
        return false;

    *context = decoded;
    return true;
}

static bool plic_decode_context_reg(uint32_t addr,
                                    uint32_t *context,
                                    uint32_t *reg)
{
    if (addr < PLIC_CONTEXT_BASE)
        return false;

    uint32_t offset = addr - PLIC_CONTEXT_BASE;
    uint32_t decoded_reg = offset % PLIC_CONTEXT_STRIDE;
    if (decoded_reg != PLIC_CONTEXT_THRESHOLD &&
        decoded_reg != PLIC_CONTEXT_CLAIM)
        return false;

    uint32_t decoded_context = offset / PLIC_CONTEXT_STRIDE;
    if (decoded_context >= 32)
        return false;

    *context = decoded_context;
    *reg = decoded_reg;
    return true;
}

void plic_update_interrupts(vm_t *vm, plic_state_t *plic)
{
    /* Update pending interrupts */
    plic->ip |= plic->active & ~plic->masked;
    plic->masked |= plic->active;
    /* Send interrupt to target */
    for (uint32_t i = 0; i < vm->n_hart; i++) {
        if (plic->ip & plic->ie[i]) {
            hart_sip_set_bits(vm->hart[i], RV_INT_SEI_BIT);
            /* Clear WFI flag when external interrupt is injected */
            hart_in_wfi_store(vm->hart[i], false);
        } else {
            hart_sip_clear_bits(vm->hart[i], RV_INT_SEI_BIT);
        }
    }
}

static bool plic_reg_read(plic_state_t *plic, uint32_t addr, uint32_t *value)
{
    /* no priority support: source priority hardwired to 1 */
    if (1 <= addr && addr <= 31)
        return true;

    if (addr == 0x400) {
        *value = plic->ip;
        return true;
    }

    uint32_t context;
    if (plic_decode_enable_context(addr, &context)) {
        *value = plic->ie[context];
        return true;
    }

    uint32_t reg;
    if (!plic_decode_context_reg(addr, &context, &reg))
        return false;

    switch (reg) {
    case PLIC_CONTEXT_THRESHOLD:
        *value = 0;
        /* no priority support: target priority threshold hardwired to 0 */
        return true;
    case PLIC_CONTEXT_CLAIM:
        /* claim */
        *value = 0;
        uint32_t candidates = plic->ip & plic->ie[context];
        if (candidates) {
            *value = (uint32_t) __builtin_ctz(candidates);
            plic->ip &= ~(UINT32_C(1) << (*value));
        }
        return true;
    default:
        return false;
    }
}

static bool plic_reg_write(plic_state_t *plic, uint32_t addr, uint32_t value)
{
    /* no priority support: source priority hardwired to 1 */
    if (1 <= addr && addr <= 31)
        return true;

    uint32_t context;
    if (plic_decode_enable_context(addr, &context)) {
        value &= ~1;
        plic->ie[context] = value;
        return true;
    }

    uint32_t reg;
    if (!plic_decode_context_reg(addr, &context, &reg))
        return false;

    switch (reg) {
    case PLIC_CONTEXT_THRESHOLD:
        /* no priority support: target priority threshold hardwired to 0 */
        return true;
    case PLIC_CONTEXT_CLAIM:
        /* completion */
        if (plic->ie[context] & (1 << value))
            plic->masked &= ~(1 << value);
        return true;
    default:
        return false;
    }
}

void plic_read(hart_t *vm,
               plic_state_t *plic,
               uint32_t addr,
               uint8_t width,
               uint32_t *value)
{
    switch (width) {
    case RV_MEM_LW:
        if (!plic_reg_read(plic, addr >> 2, value))
            vm_set_exception(vm, RV_EXC_LOAD_FAULT, vm->exc_val);
        break;
    case RV_MEM_LBU:
    case RV_MEM_LB:
    case RV_MEM_LHU:
    case RV_MEM_LH:
        vm_set_exception(vm, RV_EXC_LOAD_MISALIGN, vm->exc_val);
        return;
    default:
        vm_set_exception(vm, RV_EXC_ILLEGAL_INSN, 0);
        return;
    }
}

void plic_write(hart_t *vm,
                plic_state_t *plic,
                uint32_t addr,
                uint8_t width,
                uint32_t value)
{
    switch (width) {
    case RV_MEM_SW:
        if (!plic_reg_write(plic, addr >> 2, value))
            vm_set_exception(vm, RV_EXC_STORE_FAULT, vm->exc_val);
        break;
    case RV_MEM_SB:
    case RV_MEM_SH:
        vm_set_exception(vm, RV_EXC_STORE_MISALIGN, vm->exc_val);
        return;
    default:
        vm_set_exception(vm, RV_EXC_ILLEGAL_INSN, 0);
        return;
    }
}