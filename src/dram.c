#include "dram.h"

void dram_init(dram_t *dram, u64 size) {
    dram->size = size;
    dram->mem = (u8 *)calloc(1, size);
    if (!dram->mem) {
        fprintf(stderr, "Failed to allocate %llu bytes for DRAM\n",
                (unsigned long long)size);
        exit(1);
    }
}

void dram_free(dram_t *dram) {
    free(dram->mem);
    dram->mem = NULL;
}

void dram_load_binary(dram_t *dram, const u8 *data, u64 len, u64 offset) {
    if (offset + len > dram->size) {
        fprintf(stderr, "Binary too large for DRAM: %llu bytes at offset %llu\n",
                (unsigned long long)len, (unsigned long long)offset);
        exit(1);
    }
    memcpy(dram->mem + offset, data, len);
}

u64 dram_load(dram_t *dram, u64 addr, int size) {
    u64 val = 0;
    /* memcpy compiles to a single unaligned load on x86 for these sizes */
    switch (size) {
        case SIZE_BYTE:
            val = dram->mem[addr];
            break;
        case SIZE_HALF:
            memcpy(&val, &dram->mem[addr], 2);
            break;
        case SIZE_WORD:
            memcpy(&val, &dram->mem[addr], 4);
            break;
        case SIZE_DWORD:
            memcpy(&val, &dram->mem[addr], 8);
            break;
    }
    return val;
}

void dram_store(dram_t *dram, u64 addr, u64 value, int size) {
    switch (size) {
        case SIZE_BYTE:
            dram->mem[addr] = (u8)value;
            break;
        case SIZE_HALF:
            memcpy(&dram->mem[addr], &value, 2);
            break;
        case SIZE_WORD:
            memcpy(&dram->mem[addr], &value, 4);
            break;
        case SIZE_DWORD:
            memcpy(&dram->mem[addr], &value, 8);
            break;
    }
}
