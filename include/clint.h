#ifndef CLINT_H
#define CLINT_H

#include "types.h"

/* CLINT register offsets from CLINT_BASE */
#define CLINT_MSIP       0x0000
#define CLINT_MTIMECMP   0x4000
#define CLINT_MTIME      0xBFF8

typedef struct {
    u64 mtime;
    u64 mtimecmp;
    u32 msip;
} clint_t;

void clint_init(clint_t *clint);
u64  clint_load(clint_t *clint, u64 offset, int size);
void clint_store(clint_t *clint, u64 offset, u64 value, int size);
void clint_tick(clint_t *clint);

#endif /* CLINT_H */
