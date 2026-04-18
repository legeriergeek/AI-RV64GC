#ifndef PLIC_H
#define PLIC_H

#include "types.h"

/* PLIC supports up to 1024 interrupt sources, 2 contexts (M/S for hart 0) */
#define PLIC_MAX_SOURCE   1024
#define PLIC_MAX_CONTEXT  2

/* Context indices */
#define PLIC_CONTEXT_M  0
#define PLIC_CONTEXT_S  1

/* PLIC register offsets from PLIC_BASE */
#define PLIC_PRIORITY_BASE    0x000000
#define PLIC_PENDING_BASE     0x001000
#define PLIC_ENABLE_BASE      0x002000
#define PLIC_ENABLE_STRIDE    0x80
#define PLIC_CONTEXT_BASE     0x200000
#define PLIC_CONTEXT_STRIDE   0x1000
#define PLIC_THRESHOLD_OFFSET 0x00
#define PLIC_CLAIM_OFFSET     0x04

typedef struct {
    u32 priority[PLIC_MAX_SOURCE];
    u32 pending[PLIC_MAX_SOURCE / 32];
    u32 enable[PLIC_MAX_CONTEXT][PLIC_MAX_SOURCE / 32];
    u32 threshold[PLIC_MAX_CONTEXT];
    u32 claim[PLIC_MAX_CONTEXT];
} plic_t;

void plic_init(plic_t *plic);
u64  plic_load(plic_t *plic, u64 offset, int size);
void plic_store(plic_t *plic, u64 offset, u64 value, int size);
void plic_set_pending(plic_t *plic, int irq);
void plic_clear_pending(plic_t *plic, int irq);
u32  plic_claim(plic_t *plic, int context);
void plic_complete(plic_t *plic, int context, u32 irq);
bool plic_is_interrupting(plic_t *plic, int context);

#endif /* PLIC_H */
