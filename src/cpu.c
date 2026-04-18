#include "cpu.h"
#include "dtb.h"
#include <signal.h>
#include <termios.h>
#include <unistd.h>

/* ---- Helpers ---- */
static inline u64 sext(u64 val, int bits) {
    u64 m = 1ULL << (bits - 1);
    return (val ^ m) - m;
}
static inline u32 BITS(u32 v, int hi, int lo) { return (v >> lo) & ((1U << (hi - lo + 1)) - 1); }
static inline u32 OPCODE(u32 i) { return i & 0x7F; }
static inline u32 RD(u32 i) { return (i >> 7) & 0x1F; }
static inline u32 RS1(u32 i) { return (i >> 15) & 0x1F; }
static inline u32 RS2(u32 i) { return (i >> 20) & 0x1F; }
static inline u32 FUNCT3(u32 i) { return (i >> 12) & 0x7; }
static inline u32 FUNCT7(u32 i) { return (i >> 25) & 0x7F; }
static inline i64 IMM_I(u32 i) { return (i64)(i32)(i & 0xFFF00000) >> 20; }
static inline i64 IMM_S(u32 i) { return (i64)(i32)((i & 0xFE000000) | ((i >> 7) & 0x1F) << 20) >> 20; }
static inline i64 IMM_B(u32 i) {
    u32 v = ((i >> 31) << 12) | (((i >> 7) & 1) << 11) | (((i >> 25) & 0x3F) << 5) | (((i >> 8) & 0xF) << 1);
    return sext(v, 13);
}
static inline i64 IMM_U(u32 i) { return (i64)(i32)(i & 0xFFFFF000); }
static inline i64 IMM_J(u32 i) {
    u32 v = ((i >> 31) << 20) | (((i >> 12) & 0xFF) << 12) | (((i >> 20) & 1) << 11) | (((i >> 21) & 0x3FF) << 1);
    return sext(v, 21);
}

/* ---- FP helpers ---- */
static inline u64 box_f32(float f) { union { float f; u32 u; } c; c.f = f; return 0xFFFFFFFF00000000ULL | c.u; }
static inline float unbox_f32(u64 v) { if ((v >> 32) != 0xFFFFFFFF) return NAN; union { u32 u; float f; } c; c.u = (u32)v; return c.f; }
static inline u64 box_f64(double d) { union { double d; u64 u; } c; c.d = d; return c.u; }
static inline double unbox_f64(u64 v) { union { u64 u; double d; } c; c.u = v; return c.d; }



/* ---- CSR access ---- */
/* Mask for sstatus: the S-mode view of mstatus */
#define SSTATUS_MASK (MSTATUS_SIE | MSTATUS_SPIE | MSTATUS_UBE | MSTATUS_SPP | \
                      MSTATUS_FS | MSTATUS_XS | MSTATUS_SUM | MSTATUS_MXR | \
                      MSTATUS_UXL | MSTATUS_SD)
#define SIP_MASK (MIP_SSIP | MIP_STIP | MIP_SEIP)
#define SIE_MASK SIP_MASK

u64 csr_read(cpu_t *cpu, u16 addr) {
    switch (addr) {
        case CSR_SSTATUS:    return cpu->csrs[CSR_MSTATUS] & SSTATUS_MASK;
        case CSR_SIE:        return cpu->csrs[CSR_MIE] & SIE_MASK;
        case CSR_SIP:        return cpu->csrs[CSR_MIP] & SIP_MASK;
        case CSR_FFLAGS:     return cpu->csrs[CSR_FCSR] & 0x1F;
        case CSR_FRM:        return (cpu->csrs[CSR_FCSR] >> 5) & 0x7;
        case CSR_CYCLE:
        case CSR_INSTRET:    return cpu->instret;
        case CSR_TIME:       return cpu->bus.clint.mtime;
        case CSR_MISA:       return cpu->csrs[CSR_MISA];
        case CSR_MVENDORID:  return 0;
        case CSR_MARCHID:    return 0;
        case CSR_MIMPID:     return 0;
        case CSR_MHARTID:    return 0;
        default:
            /* Block Sstc CSRs to prevent OpenSBI from detecting Sstc extension */
            if ((addr & 0xFFF) == 0x14D || (addr & 0xFFF) == 0x15D ||
                (addr & 0xFFF) == 0x24D || (addr & 0xFFF) == 0x25D) {
                return 0;
            }
            /* menvcfg: mask off STCE bit (63) so Sstc probe fails */
            if ((addr & 0xFFF) == 0x30A) {
                return cpu->csrs[addr & 0xFFF] & ~(1ULL << 63);
            }
            return cpu->csrs[addr & 0xFFF];
    }
}

void cpu_update_mmu_state(cpu_t *cpu) {
    u64 satp = cpu->csrs[CSR_SATP];
    u64 mstatus = cpu->csrs[CSR_MSTATUS];
    u32 mode = (u32)(satp >> 60);
    u32 sum = (mstatus & MSTATUS_SUM) ? 1 : 0;
    u32 mxr = (mstatus & MSTATUS_MXR) ? 1 : 0;
    u32 mprv = (mstatus & MSTATUS_MPRV) ? 1 : 0;
    u32 mpp = (u32)((mstatus >> 11) & 3);
    /* mmu_state: priv[0:1], mode[2:5], sum[6], mxr[7], mprv[8], mpp[9:10] */
    cpu->mmu_state = (u32)cpu->priv | (mode << 2) | (sum << 6) | (mxr << 7) | (mprv << 8) | (mpp << 9);
    cpu->icache_valid = false;
}

void csr_write(cpu_t *cpu, u16 addr, u64 value) {
    switch (addr) {
        case CSR_SSTATUS:
            cpu->csrs[CSR_MSTATUS] = (cpu->csrs[CSR_MSTATUS] & ~SSTATUS_MASK) | (value & SSTATUS_MASK);
            cpu_update_mmu_state(cpu);
            break;
        case CSR_SIE:
            cpu->csrs[CSR_MIE] = (cpu->csrs[CSR_MIE] & ~SIE_MASK) | (value & SIE_MASK);
            break;
        case CSR_SIP:
            cpu->csrs[CSR_MIP] = (cpu->csrs[CSR_MIP] & ~MIP_SSIP) | (value & MIP_SSIP);
            break;
        case CSR_FFLAGS:
            cpu->csrs[CSR_FCSR] = (cpu->csrs[CSR_FCSR] & ~0x1FULL) | (value & 0x1F);
            break;
        case CSR_FRM:
            cpu->csrs[CSR_FCSR] = (cpu->csrs[CSR_FCSR] & ~0xE0ULL) | ((value & 0x7) << 5);
            break;
        case CSR_MISA:
            break; /* Read-only in our impl */
        case CSR_MVENDORID: case CSR_MARCHID: case CSR_MIMPID: case CSR_MHARTID:
            break; /* Read-only */
        case CSR_SATP: {
            u64 mode = value >> 60;
            if (mode == 0 || mode == 8) {
                if (cpu->csrs[CSR_SATP] != value) {
                    cpu->csrs[CSR_SATP] = value;
                    cpu_update_mmu_state(cpu);
                    mmu_tlb_flush(cpu);
                }
            }
            break;
        }
        case 0x14D: /* stimecmp */
            cpu->csrs[0x14D] = value;
            break;
        default:
            cpu->csrs[addr & 0xFFF] = value;
            if ((addr & 0xFFF) == CSR_MSTATUS) cpu_update_mmu_state(cpu);
            break;
    }
}

extern bool g_verbose;

/* ---- Trap handling ---- */
void cpu_take_trap(cpu_t *cpu, u64 cause, u64 tval, bool is_interrupt) {
    u64 mcause = is_interrupt ? (cause | (1ULL << 63)) : cause;
    /* Diagnostic: log U-mode illegal instruction traps */
    if (!is_interrupt && cause == 2 && cpu->priv == PRIV_U) {
        fprintf(stderr, "[rvemu] U-MODE ILLEGAL INST at pc=%llx inst=%08llx ra=%llx sp=%llx gp=%llx\n",
                (unsigned long long)cpu->pc, (unsigned long long)tval,
                (unsigned long long)cpu->regs[1], (unsigned long long)cpu->regs[2],
                (unsigned long long)cpu->regs[3]);
        fprintf(stderr, "  a0=%llx a1=%llx a2=%llx a3=%llx a4=%llx a5=%llx a6=%llx a7=%llx\n",
                (unsigned long long)cpu->regs[10], (unsigned long long)cpu->regs[11],
                (unsigned long long)cpu->regs[12], (unsigned long long)cpu->regs[13],
                (unsigned long long)cpu->regs[14], (unsigned long long)cpu->regs[15],
                (unsigned long long)cpu->regs[16], (unsigned long long)cpu->regs[17]);
        fprintf(stderr, "  t0=%llx t1=%llx t2=%llx t3=%llx t4=%llx t5=%llx t6=%llx\n",
                (unsigned long long)cpu->regs[5], (unsigned long long)cpu->regs[6],
                (unsigned long long)cpu->regs[7], (unsigned long long)cpu->regs[28],
                (unsigned long long)cpu->regs[29], (unsigned long long)cpu->regs[30],
                (unsigned long long)cpu->regs[31]);
    }
    if (g_verbose) {
        fprintf(stderr, "core 0: trap %s cause %llx tval %llx pc %llx satp %llx mstatus %llx priv %d\n",
                is_interrupt ? "int" : "exc", (unsigned long long)mcause, (unsigned long long)tval, (unsigned long long)cpu->pc,
                (unsigned long long)cpu->csrs[CSR_SATP], (unsigned long long)cpu->csrs[CSR_MSTATUS], cpu->priv);
    }
    u64 mstatus = cpu->csrs[CSR_MSTATUS];
    u8 prev_priv = cpu->priv;

    /* Check delegation */
    bool delegate = false;
    if (cpu->priv <= PRIV_S) {
        if (is_interrupt) {
            delegate = (cpu->csrs[CSR_MIDELEG] >> cause) & 1;
        } else {
            delegate = (cpu->csrs[CSR_MEDELEG] >> cause) & 1;
        }
    }

    if (delegate) {
        /* Trap to S-mode */
        cpu->csrs[CSR_SEPC] = cpu->pc;
        cpu->csrs[CSR_SCAUSE] = mcause;
        cpu->csrs[CSR_STVAL] = tval;
        /* Set SPIE = SIE, clear SIE, set SPP */
        u64 sie = (mstatus >> 1) & 1;
        mstatus = (mstatus & ~MSTATUS_SPIE) | (sie << 5);
        mstatus &= ~MSTATUS_SIE;
        mstatus = (mstatus & ~MSTATUS_SPP) | ((u64)prev_priv << 8);
        cpu->csrs[CSR_MSTATUS] = mstatus;
        cpu->priv = PRIV_S;
        u64 stvec = cpu->csrs[CSR_STVEC];
        u64 mode = stvec & 3;
        u64 base = stvec & ~3ULL;
        cpu->pc = (mode == 1 && is_interrupt) ? base + cause * 4 : base;
    } else {
        /* Trap to M-mode */
        cpu->csrs[CSR_MEPC] = cpu->pc;
        cpu->csrs[CSR_MCAUSE] = mcause;
        cpu->csrs[CSR_MTVAL] = tval;
        u64 m_ie = (mstatus >> 3) & 1;
        mstatus = (mstatus & ~MSTATUS_MPIE) | (m_ie << 7);
        mstatus &= ~MSTATUS_MIE;
        mstatus = (mstatus & ~MSTATUS_MPP) | ((u64)prev_priv << 11);
        cpu->csrs[CSR_MSTATUS] = mstatus;
        cpu->priv = PRIV_M;
        u64 mtvec = cpu->csrs[CSR_MTVEC];
        u64 mode = mtvec & 3;
        u64 base = mtvec & ~3ULL;
        cpu->pc = (mode == 1 && is_interrupt) ? base + cause * 4 : base;
    }
    cpu_update_mmu_state(cpu);
}

/* ---- Interrupt check ---- */
void cpu_check_interrupt(cpu_t *cpu) {
    u64 mstatus = cpu->csrs[CSR_MSTATUS];
    u64 mip = cpu->csrs[CSR_MIP];
    u64 mie = cpu->csrs[CSR_MIE];
    u64 pending = mip & mie;
    if (pending == 0) return;

    /* Wake up from WFI if an enabled interrupt is pending, even if global enable is off */
    if (cpu->wfi) cpu->wfi = false;

    /* M-mode interrupts */
    bool m_enabled = (cpu->priv < PRIV_M) || (cpu->priv == PRIV_M && (mstatus & MSTATUS_MIE));
    /* S-mode interrupts */
    bool s_enabled = (cpu->priv < PRIV_S) || (cpu->priv == PRIV_S && (mstatus & MSTATUS_SIE));

    /* Check interrupts in priority order */
    static const int irq_order[] = { IRQ_M_EXT, IRQ_M_SOFT, IRQ_M_TIMER, IRQ_S_EXT, IRQ_S_SOFT, IRQ_S_TIMER };
    for (int i = 0; i < 6; i++) {
        int irq = irq_order[i];
        if (!(pending & (1ULL << irq))) continue;
        bool delegated = (cpu->csrs[CSR_MIDELEG] >> irq) & 1;
        if (delegated) {
            if (!s_enabled) continue;
        } else {
            if (!m_enabled) continue;
        }
        cpu_take_trap(cpu, irq, 0, true);
        return;
    }
}

/* ---- Memory access through MMU ---- */
static inline mmu_result_t mmu_translate_inline(cpu_t *cpu, u64 vaddr, access_type_t access) {
    u32 state = cpu->mmu_state;
    u8 priv = state & 3;
    u32 mode = (state >> 2) & 0xF;

    if (access != ACCESS_EXEC && (state & (1 << 8))) /* MPRV */
        priv = (state >> 9) & 3; /* MPP */

    if (priv == PRIV_M || mode == SATP_MODE_BARE) {
        u8 *hp = (vaddr >= DRAM_BASE && vaddr < DRAM_BASE + DRAM_SIZE) ? &cpu->bus.dram.mem[vaddr - DRAM_BASE] : NULL;
        return (mmu_result_t){ .paddr = vaddr, .host_ptr = hp, .exception = false };
    }

    u64 vpn_query = vaddr >> 12;
    tlb_entry_t *entry = &cpu->tlb[vpn_query & (TLB_SIZE - 1)];

    if (entry->valid && entry->vpn == vpn_query) {
        u32 pte = entry->flags;
        bool fault = false;
        if (priv == PRIV_S) {
            if ((pte & PTE_U) && (access == ACCESS_EXEC || !(state & (1 << 6)))) fault = true; /* SUM */
        } else if (priv == PRIV_U) {
            if (!(pte & PTE_U)) fault = true;
        }

        if (!fault) {
            if (access == ACCESS_EXEC) { if (!(pte & PTE_X)) fault = true; }
            else if (access == ACCESS_LOAD) { if (!(pte & PTE_R) && (!(state & (1 << 7)) || !(pte & PTE_X))) fault = true; } /* MXR */
            else { if (!(pte & PTE_R) || !(pte & PTE_W) || !(pte & PTE_D)) fault = true; }
        }
        
        if (!fault && (pte & PTE_A)) {
            u64 pa = (entry->ppn << 12) | (vaddr & 0xFFF);
            u8 *hp = (pa >= DRAM_BASE && pa < DRAM_BASE + DRAM_SIZE) ? &cpu->bus.dram.mem[pa - DRAM_BASE] : NULL;
            return (mmu_result_t){ .paddr = pa, .host_ptr = hp, .exception = false };
        }
    }
    return mmu_translate(cpu, vaddr, access);
}

load_result_t cpu_load(cpu_t *cpu, u64 addr, int size) {
    mmu_result_t tr = mmu_translate_inline(cpu, addr, ACCESS_LOAD);
    if (tr.exception) {
        return (load_result_t){ .value = 0, .exception = true, .exc_code = tr.exc_code, .exc_val = tr.exc_val };
    }
    if (tr.host_ptr) {
        if (size == SIZE_BYTE)  return (load_result_t){ .value = *(u8*)tr.host_ptr, .exception = false };
        if (size == SIZE_HALF)  return (load_result_t){ .value = *(u16*)tr.host_ptr, .exception = false };
        if (size == SIZE_WORD)  return (load_result_t){ .value = *(u32*)tr.host_ptr, .exception = false };
        return (load_result_t){ .value = *(u64*)tr.host_ptr, .exception = false };
    }
    return bus_load(&cpu->bus, tr.paddr, size);
}

store_result_t cpu_store(cpu_t *cpu, u64 addr, u64 value, int size) {
    mmu_result_t tr = mmu_translate_inline(cpu, addr, ACCESS_STORE);
    if (tr.exception) {
        return (store_result_t){ .exception = true, .exc_code = tr.exc_code, .exc_val = tr.exc_val };
    }
    if (tr.host_ptr) {
        if (size == SIZE_BYTE)  { *(u8*)tr.host_ptr = (u8)value; return (store_result_t){0}; }
        if (size == SIZE_HALF)  { *(u16*)tr.host_ptr = (u16)value; return (store_result_t){0}; }
        if (size == SIZE_WORD)  { *(u32*)tr.host_ptr = (u32)value; return (store_result_t){0}; }
        *(u64*)tr.host_ptr = (u64)value; return (store_result_t){0};
    }
    return bus_store(&cpu->bus, tr.paddr, value, size, cpu);
}

u64 cpu_fetch(cpu_t *cpu) {
    mmu_result_t tr = mmu_translate(cpu, cpu->pc, ACCESS_EXEC);
    if (tr.exception) {
        cpu_take_trap(cpu, tr.exc_code, tr.exc_val, false);
        return 0;
    }
    load_result_t lr = bus_load(&cpu->bus, tr.paddr, SIZE_WORD);
    if (lr.exception) {
        cpu_take_trap(cpu, EXC_INST_ACCESS_FAULT, cpu->pc, false);
        return 0;
    }
    return lr.value;
}

/* ---- Init ---- */
void cpu_init(cpu_t *cpu, const char *disk_path, const char *tap_name) {
    memset(cpu, 0, sizeof(cpu_t));
    bus_init(&cpu->bus, disk_path, tap_name);
    cpu->pc = DRAM_BASE;
    cpu->priv = PRIV_M;
    cpu->regs[2] = DRAM_BASE + DRAM_SIZE; /* sp */
    /* misa: RV64IMAFDC = bits I(8) M(12) A(0) F(5) D(3) C(2) + MXL=2(64-bit) + S(18) + U(20) */
    cpu->csrs[CSR_MISA] = (2ULL << 62) | (1 << 0) | (1 << 2) | (1 << 3) | (1 << 5) | (1 << 8) | (1 << 12) | (1 << 18) | (1 << 20);
    /* mstatus: set UXL=2 and SXL=2 for 64-bit */
    cpu->csrs[CSR_MSTATUS] = (2ULL << 32) | (2ULL << 34);
    cpu_update_mmu_state(cpu);
}

void cpu_free(cpu_t *cpu) { bus_free(&cpu->bus); }

void cpu_load_binary(cpu_t *cpu, const u8 *data, u64 len) {
    dram_load_binary(&cpu->bus.dram, data, len, 0);
}

/* ---- Instruction execution (forward declaration, defined in execute.c) ---- */
extern void cpu_execute(cpu_t *cpu, u32 inst);
extern void cpu_execute_compressed(cpu_t *cpu, u16 inst);

/* ---- Update PLIC/interrupt signals ---- */
static void cpu_update_devices(cpu_t *cpu) {
    /* UART interrupt */
    if (uart_is_interrupting(&cpu->bus.uart))
        plic_set_pending(&cpu->bus.plic, UART_IRQ);
    /* Virtio interrupt */
    if (virtio_is_interrupting(&cpu->bus.virtio))
        plic_set_pending(&cpu->bus.plic, VIRTIO_IRQ);
    /* Virtio net interrupt & polling */
    virtio_net_poll(&cpu->bus.virtio_net, cpu);
    if (virtio_net_is_interrupting(&cpu->bus.virtio_net))
        plic_set_pending(&cpu->bus.plic, VIRTNET_IRQ);
    /* Virtio RNG interrupt */
    if (virtio_is_interrupting(&cpu->bus.virtio_rng))
        plic_set_pending(&cpu->bus.plic, VIRTRNG_IRQ);
    /* PLIC → MIP */
    if (plic_is_interrupting(&cpu->bus.plic, PLIC_CONTEXT_M))
        cpu->csrs[CSR_MIP] |= MIP_MEIP;
    else
        cpu->csrs[CSR_MIP] &= ~MIP_MEIP;
    if (plic_is_interrupting(&cpu->bus.plic, PLIC_CONTEXT_S))
        cpu->csrs[CSR_MIP] |= MIP_SEIP;
    else
        cpu->csrs[CSR_MIP] &= ~MIP_SEIP;
    /* CLINT → MIP */
    if (cpu->bus.clint.msip)
        cpu->csrs[CSR_MIP] |= MIP_MSIP;
    else
        cpu->csrs[CSR_MIP] &= ~MIP_MSIP;
    
    /* Machine-mode timer interrupt */
    if (cpu->bus.clint.mtime >= cpu->bus.clint.mtimecmp)
        cpu->csrs[CSR_MIP] |= MIP_MTIP;
    else
        cpu->csrs[CSR_MIP] &= ~MIP_MTIP;

    /* Supervisor-mode timer interrupt (Sstc stimecmp) */
    /* stimecmp is only active if menvcfg.STCE (bit 63) is set */
    if ((cpu->csrs[0x30A] >> 63) & 1) {
        if (cpu->bus.clint.mtime >= cpu->csrs[0x14D])
            cpu->csrs[CSR_MIP] |= MIP_STIP;
        else
            cpu->csrs[CSR_MIP] &= ~MIP_STIP;
    }
}

void cpu_step(cpu_t *cpu) {
    /* Skip vdso_init — it panics because vdso_start is zeroed (dummy build). */
    if (cpu->pc == 0xffffffff80a041b2) {
        cpu->pc = cpu->regs[1];
        return;
    }

    if (cpu->wfi) {
        if ((cpu->csrs[CSR_MIP] & cpu->csrs[CSR_MIE]) != 0) {
            cpu->wfi = false;
        } else {
            cpu->bus.clint.mtime++;
            cpu_update_devices(cpu);
            cpu_check_interrupt(cpu);
            return;
        }
    }

    /* Instruction Fetch Fast-Path (IPC + DDA) */
    u64 pc = cpu->pc;
    u64 vpn = pc >> 12;
    u8 *host_ptr = NULL;

    if (__builtin_expect(cpu->icache_valid && vpn == cpu->icache_vpn, 1)) {
        host_ptr = cpu->icache_host_ptr + (pc & 0xFFF);
    } else {
        mmu_result_t tr = mmu_translate_inline(cpu, pc, ACCESS_EXEC);
        if (tr.exception) {
            cpu_take_trap(cpu, tr.exc_code, tr.exc_val, false);
            return;
        }
        if (tr.host_ptr) {
            cpu->icache_vpn = vpn;
            cpu->icache_host_ptr = tr.host_ptr - (pc & 0xFFF);
            cpu->icache_valid = true;
            host_ptr = tr.host_ptr;
        } else {
            /* Non-DRAM access: slow path */
            load_result_t lr = bus_load(&cpu->bus, tr.paddr, SIZE_HALF);
            if (lr.exception) { cpu_take_trap(cpu, EXC_INST_ACCESS_FAULT, pc, false); return; }
            u16 low = (u16)lr.value;
            if ((low & 3) != 3) {
                cpu->pc += 2;
                cpu_execute_compressed(cpu, low);
            } else {
                mmu_result_t tr2 = mmu_translate_inline(cpu, pc + 2, ACCESS_EXEC);
                if (tr2.exception) { cpu_take_trap(cpu, tr2.exc_code, tr2.exc_val, false); return; }
                load_result_t lr2 = bus_load(&cpu->bus, tr2.paddr, SIZE_HALF);
                if (lr2.exception) { cpu_take_trap(cpu, EXC_INST_ACCESS_FAULT, pc + 2, false); return; }
                cpu->pc += 4;
                cpu_execute(cpu, (u32)low | ((u32)lr2.value << 16));
            }
            goto step_done;
        }
    }

    /* Fast path execution (DRAM resident page) */
    u32 inst;
    int inst_len = 4;
    u16 low = *(u16*)host_ptr;
    if ((low & 3) != 3) {
        inst = low;
        inst_len = 2;
    } else {
        /* check for page boundary cross */
        if (__builtin_expect((pc & 0xFFF) <= 0xFFC, 1)) {
            inst = *(u32*)host_ptr;
        } else {
            /* cross-page 32-bit instruction */
            mmu_result_t tr2 = mmu_translate_inline(cpu, pc + 2, ACCESS_EXEC);
            if (tr2.exception) { cpu_take_trap(cpu, tr2.exc_code, tr2.exc_val, false); return; }
            load_result_t lr2 = bus_load(&cpu->bus, tr2.paddr, SIZE_HALF);
            if (lr2.exception) { cpu_take_trap(cpu, EXC_INST_ACCESS_FAULT, pc + 2, false); return; }
            inst = (u32)low | ((u32)lr2.value << 16);
        }
    }

    cpu->pc += inst_len;
    if (inst_len == 2) cpu_execute_compressed(cpu, (u16)inst);
    else cpu_execute(cpu, inst);

step_done:
    cpu->regs[0] = 0; /* x0 is always 0 */
    cpu->instret++;

    /* Batched updates for performance */
    if (__builtin_expect((cpu->instret & 255) == 0, 0)) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        cpu->bus.clint.mtime = (u64)ts.tv_sec * 10000000ULL + ts.tv_nsec / 100;
        cpu_update_devices(cpu);
        cpu_check_interrupt(cpu);

        if (__builtin_expect(g_mips_report && (cpu->instret % 50000000) == 0, 0)) {
            static struct timespec last_time;
            static u64 last_instret = 0;
            struct timespec current_time;
            clock_gettime(CLOCK_MONOTONIC, &current_time);
            
            if (last_instret != 0) {
                double elapsed = (current_time.tv_sec - last_time.tv_sec) + 
                                 (current_time.tv_nsec - last_time.tv_nsec) / 1e9;
                if (elapsed > 0) {
                    double mips = (cpu->instret - last_instret) / (elapsed * 1000000.0);
                    fprintf(stderr, "\r[rvemu] Performance: %.2f MIPS (%.2f MHz)\x1b[K", mips, mips);
                    fflush(stderr);
                }
            }
            last_time = current_time;
            last_instret = cpu->instret;
        }
    }
}

/* ---- Terminal setup ---- */
static struct termios orig_termios;
static void restore_term(void) { tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios); }

static void setup_term(void) {
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(restore_term);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
    raw.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
    raw.c_oflag &= ~(OPOST);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
}

/* ---- Main loop ---- */
void cpu_loop(cpu_t *cpu) {
    setup_term();
    while (1) {
        for (int i = 0; i < 128; i++) {
            cpu_step(cpu);
        }
    }
}
