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
    switch (size) {
        case SIZE_BYTE:
            val = dram->mem[addr];
            break;
        case SIZE_HALF:
            val = (u64)dram->mem[addr]
                | ((u64)dram->mem[addr + 1] << 8);
            break;
        case SIZE_WORD:
            val = (u64)dram->mem[addr]
                | ((u64)dram->mem[addr + 1] << 8)
                | ((u64)dram->mem[addr + 2] << 16)
                | ((u64)dram->mem[addr + 3] << 24);
            break;
        case SIZE_DWORD:
            val = (u64)dram->mem[addr]
                | ((u64)dram->mem[addr + 1] << 8)
                | ((u64)dram->mem[addr + 2] << 16)
                | ((u64)dram->mem[addr + 3] << 24)
                | ((u64)dram->mem[addr + 4] << 32)
                | ((u64)dram->mem[addr + 5] << 40)
                | ((u64)dram->mem[addr + 6] << 48)
                | ((u64)dram->mem[addr + 7] << 56);
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
            dram->mem[addr]     = (u8)(value);
            dram->mem[addr + 1] = (u8)(value >> 8);
            break;
        case SIZE_WORD:
            dram->mem[addr]     = (u8)(value);
            dram->mem[addr + 1] = (u8)(value >> 8);
            dram->mem[addr + 2] = (u8)(value >> 16);
            dram->mem[addr + 3] = (u8)(value >> 24);
            break;
        case SIZE_DWORD:
            dram->mem[addr]     = (u8)(value);
            dram->mem[addr + 1] = (u8)(value >> 8);
            dram->mem[addr + 2] = (u8)(value >> 16);
            dram->mem[addr + 3] = (u8)(value >> 24);
            dram->mem[addr + 4] = (u8)(value >> 32);
            dram->mem[addr + 5] = (u8)(value >> 40);
            dram->mem[addr + 6] = (u8)(value >> 48);
            dram->mem[addr + 7] = (u8)(value >> 56);
            break;
    }
}
