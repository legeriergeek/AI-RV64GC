#ifndef CPU_H
#define CPU_H

#include "types.h"
#include "bus.h"
#include "mmu.h"

typedef struct cpu {
    /* Integer registers */
    u64 regs[32];
    /* Floating-point registers (stored as u64 for bit manipulation) */
    u64 fregs[32];
    /* Program counter */
    u64 pc;
    /* Software TLB */
    tlb_entry_t tlb[TLB_SIZE];

    /* Privilege mode */
    u8  priv;
    /* Memory bus */
    bus_t bus;

    /* CSR registers */
    u64 csrs[4096];

    /* Instruction counter */
    u64 instret;

    /* Wait-for-interrupt flag */
    bool wfi;

    /* Whether an exception/interrupt is pending from this cycle */
    bool exc_pending;
    u64  exc_code;
    u64  exc_val;
    bool exc_is_interrupt;

    /* Cached MMU state for fast translation */
    u32  mmu_state; /* Combined priv, satp_mode, SUM, MXR, MPRV */

    /* Instruction Page Cache (IPC) */
    u64  icache_vpn;
    u8  *icache_host_ptr;
    bool icache_valid;
} cpu_t;

/* Lifecycle */
void cpu_init(cpu_t *cpu, const char *disk_path, const char *tap_name);
void cpu_free(cpu_t *cpu);
void cpu_load_binary(cpu_t *cpu, const u8 *data, u64 len);

/* Execution */
void cpu_step(cpu_t *cpu);
void cpu_loop(cpu_t *cpu);

/* CSR access */
u64  csr_read(cpu_t *cpu, u16 addr);
void csr_write(cpu_t *cpu, u16 addr, u64 value);

/* Trap handling */
void cpu_take_trap(cpu_t *cpu, u64 cause, u64 tval, bool is_interrupt);
void cpu_check_interrupt(cpu_t *cpu);

/* Memory access through MMU */
load_result_t  cpu_load(cpu_t *cpu, u64 addr, int size);
store_result_t cpu_store(cpu_t *cpu, u64 addr, u64 value, int size);

/* Fetch instruction */
u64 cpu_fetch(cpu_t *cpu);

/* MMU Helper */
void cpu_update_mmu_state(cpu_t *cpu);

#endif /* CPU_H */
