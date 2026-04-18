#include "plic.h"

void plic_init(plic_t *plic) {
    memset(plic, 0, sizeof(plic_t));
}

u64 plic_load(plic_t *plic, u64 offset, int size) {
    (void)size;

    /* Source priorities: 0x000000 - 0x000FFF */
    if (offset < 0x1000) {
        u32 source = offset / 4;
        if (source < PLIC_MAX_SOURCE)
            return plic->priority[source];
        return 0;
    }

    /* Pending bits: 0x001000 - 0x001FFF */
    if (offset >= PLIC_PENDING_BASE && offset < PLIC_PENDING_BASE + 0x80) {
        u32 idx = (offset - PLIC_PENDING_BASE) / 4;
        if (idx < PLIC_MAX_SOURCE / 32)
            return plic->pending[idx];
        return 0;
    }

    /* Enable bits: 0x002000 - 0x1FFFFF */
    if (offset >= PLIC_ENABLE_BASE && offset < PLIC_CONTEXT_BASE) {
        u32 context = (offset - PLIC_ENABLE_BASE) / PLIC_ENABLE_STRIDE;
        u32 idx = ((offset - PLIC_ENABLE_BASE) % PLIC_ENABLE_STRIDE) / 4;
        if (context < PLIC_MAX_CONTEXT && idx < PLIC_MAX_SOURCE / 32)
            return plic->enable[context][idx];
        return 0;
    }

    /* Context-specific: threshold and claim */
    if (offset >= PLIC_CONTEXT_BASE) {
        u32 context = (offset - PLIC_CONTEXT_BASE) / PLIC_CONTEXT_STRIDE;
        u32 ctx_offset = (offset - PLIC_CONTEXT_BASE) % PLIC_CONTEXT_STRIDE;
        if (context < PLIC_MAX_CONTEXT) {
            if (ctx_offset == PLIC_THRESHOLD_OFFSET) {
                return plic->threshold[context];
            } else if (ctx_offset == PLIC_CLAIM_OFFSET) {
                return plic_claim(plic, context);
            }
        }
    }

    return 0;
}

void plic_store(plic_t *plic, u64 offset, u64 value, int size) {
    (void)size;

    /* Source priorities */
    if (offset < 0x1000) {
        u32 source = offset / 4;
        if (source < PLIC_MAX_SOURCE)
            plic->priority[source] = (u32)value;
        return;
    }

    /* Pending bits (generally read-only, but some impls allow) */
    if (offset >= PLIC_PENDING_BASE && offset < PLIC_PENDING_BASE + 0x80) {
        return; /* read-only */
    }

    /* Enable bits */
    if (offset >= PLIC_ENABLE_BASE && offset < PLIC_CONTEXT_BASE) {
        u32 context = (offset - PLIC_ENABLE_BASE) / PLIC_ENABLE_STRIDE;
        u32 idx = ((offset - PLIC_ENABLE_BASE) % PLIC_ENABLE_STRIDE) / 4;
        if (context < PLIC_MAX_CONTEXT && idx < PLIC_MAX_SOURCE / 32)
            plic->enable[context][idx] = (u32)value;
        return;
    }

    /* Context-specific: threshold and complete */
    if (offset >= PLIC_CONTEXT_BASE) {
        u32 context = (offset - PLIC_CONTEXT_BASE) / PLIC_CONTEXT_STRIDE;
        u32 ctx_offset = (offset - PLIC_CONTEXT_BASE) % PLIC_CONTEXT_STRIDE;
        if (context < PLIC_MAX_CONTEXT) {
            if (ctx_offset == PLIC_THRESHOLD_OFFSET) {
                plic->threshold[context] = (u32)value;
            } else if (ctx_offset == PLIC_CLAIM_OFFSET) {
                /* Complete: the kernel signals it finished the interrupt. 
                 * We MUST NOT clear the pending bit here, because a new interrupt
                 * could have arrived while the kernel was running the handler! */
                // (void)value;
            }
        }
    }
}

void plic_set_pending(plic_t *plic, int irq) {
    if (irq > 0 && irq < PLIC_MAX_SOURCE) {
        plic->pending[irq / 32] |= (1U << (irq % 32));
    }
}

void plic_clear_pending(plic_t *plic, int irq) {
    if (irq > 0 && irq < PLIC_MAX_SOURCE) {
        plic->pending[irq / 32] &= ~(1U << (irq % 32));
    }
}

u32 plic_claim(plic_t *plic, int context) {
    /* Find highest-priority pending & enabled interrupt */
    u32 best_irq = 0;
    u32 best_prio = 0;

    for (int i = 1; i < PLIC_MAX_SOURCE; i++) {
        u32 word = i / 32;
        u32 bit  = i % 32;

        if (!(plic->pending[word] & (1U << bit)))
            continue;
        if (!(plic->enable[context][word] & (1U << bit)))
            continue;
        if (plic->priority[i] <= plic->threshold[context])
            continue;
        if (plic->priority[i] > best_prio) {
            best_prio = plic->priority[i];
            best_irq = i;
        }
    }

    if (best_irq > 0) {
        /* Clear pending bit on claim */
        plic_clear_pending(plic, best_irq);
    }

    return best_irq;
}

void plic_complete(plic_t *plic, int context, u32 irq) {
    (void)context;
    (void)plic;
    (void)irq;
    /* Completion: nothing specific needed for our simple implementation */
}

bool plic_is_interrupting(plic_t *plic, int context) {
    for (int i = 1; i < PLIC_MAX_SOURCE; i++) {
        u32 word = i / 32;
        u32 bit  = i % 32;

        if (!(plic->pending[word] & (1U << bit)))
            continue;
        if (!(plic->enable[context][word] & (1U << bit)))
            continue;
        if (plic->priority[i] <= plic->threshold[context])
            continue;
        return true;
    }
    return false;
}
