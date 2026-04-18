#ifndef MMU_H
#define MMU_H

#include "types.h"

/* Forward declaration */
struct cpu;

/* Access type for MMU translation */
typedef enum {
    ACCESS_LOAD,
    ACCESS_STORE,
    ACCESS_EXEC
} access_type_t;

/* TLB Size: must be power of 2 */
#define TLB_SIZE 4096

typedef struct {
    u64 vpn;    /* Virtual Page Number (vaddr >> 12) */
    u64 ppn;    /* Physical Page Number (paddr >> 12) */
    u32 flags;  /* Original PTE flags */
    bool valid;
} tlb_entry_t;

/* MMU translation result */
typedef struct {
    u64  paddr;
    u8  *host_ptr;  /* Direct pointer to host memory (e.g. DRAM) or NULL */
    bool exception;
    u64  exc_code;
    u64  exc_val;
} mmu_result_t;

mmu_result_t mmu_translate(struct cpu *cpu, u64 vaddr, access_type_t access);
void mmu_tlb_flush(struct cpu *cpu);

#endif /* MMU_H */
