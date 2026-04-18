#ifndef DRAM_H
#define DRAM_H

#include "types.h"

typedef struct {
    u8 *mem;
    u64 size;
} dram_t;

void dram_init(dram_t *dram, u64 size);
void dram_free(dram_t *dram);
void dram_load_binary(dram_t *dram, const u8 *data, u64 len, u64 offset);

u64  dram_load(dram_t *dram, u64 addr, int size);
void dram_store(dram_t *dram, u64 addr, u64 value, int size);

#endif /* DRAM_H */
