#include "clint.h"

void clint_init(clint_t *clint) {
    clint->mtime = 0;
    clint->mtimecmp = 0;
    clint->msip = 0;
}

u64 clint_load(clint_t *clint, u64 offset, int size) {
    (void)size;
    switch (offset) {
        case CLINT_MSIP:
            return clint->msip;
        case CLINT_MTIMECMP:
            return clint->mtimecmp;
        case CLINT_MTIMECMP + 4:
            return clint->mtimecmp >> 32;
        case CLINT_MTIME:
            return clint->mtime;
        case CLINT_MTIME + 4:
            return clint->mtime >> 32;
        default:
            return 0;
    }
}

void clint_store(clint_t *clint, u64 offset, u64 value, int size) {
    switch (offset) {
        case CLINT_MSIP:
            clint->msip = (u32)(value & 1);
            break;
        case CLINT_MTIMECMP:
            if (size == SIZE_DWORD) {
                clint->mtimecmp = value;
            } else {
                clint->mtimecmp = (clint->mtimecmp & 0xFFFFFFFF00000000ULL) | (value & 0xFFFFFFFF);
            }
            break;
        case CLINT_MTIMECMP + 4:
            clint->mtimecmp = (clint->mtimecmp & 0x00000000FFFFFFFFULL) | ((value & 0xFFFFFFFF) << 32);
            break;
        case CLINT_MTIME:
            if (size == SIZE_DWORD) {
                clint->mtime = value;
            } else {
                clint->mtime = (clint->mtime & 0xFFFFFFFF00000000ULL) | (value & 0xFFFFFFFF);
            }
            break;
        case CLINT_MTIME + 4:
            clint->mtime = (clint->mtime & 0x00000000FFFFFFFFULL) | ((value & 0xFFFFFFFF) << 32);
            break;
        default:
            break;
    }
}

void clint_tick(clint_t *clint) {
    clint->mtime++;
}
