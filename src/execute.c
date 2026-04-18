#include "cpu.h"
#include <math.h>
#include <fenv.h>

/* Helpers from cpu.c */
static inline u64 sext(u64 val, int bits) { u64 m = 1ULL << (bits - 1); return (val ^ m) - m; }
static inline u32 OPCODE(u32 i) { return i & 0x7F; }
static inline u32 RD(u32 i) { return (i >> 7) & 0x1F; }
static inline u32 RS1(u32 i) { return (i >> 15) & 0x1F; }
static inline u32 RS2(u32 i) { return (i >> 20) & 0x1F; }
static inline u32 FUNCT3(u32 i) { return (i >> 12) & 0x7; }
static inline u32 FUNCT7(u32 i) { return (i >> 25) & 0x7F; }
static inline i64 IMM_I(u32 i) { return (i64)(i32)(i & 0xFFF00000) >> 20; }
static inline i64 IMM_S(u32 i) {
    return (i64)(i32)((i & 0xFE000000) | ((i >> 7) & 0x1F) << 20) >> 20;
}
static inline i64 IMM_B(u32 i) {
    u32 v = ((i >> 31) << 12) | (((i >> 7) & 1) << 11) | (((i >> 25) & 0x3F) << 5) | (((i >> 8) & 0xF) << 1);
    return sext(v, 13);
}
static inline i64 IMM_U(u32 i) { return (i64)(i32)(i & 0xFFFFF000); }
static inline i64 IMM_J(u32 i) {
    u32 v = ((i >> 31) << 20) | (((i >> 12) & 0xFF) << 12) | (((i >> 20) & 1) << 11) | (((i >> 21) & 0x3FF) << 1);
    return sext(v, 21);
}
static inline u64 box_f32(float f) { union { float f; u32 u; } c; c.f = f; return 0xFFFFFFFF00000000ULL | c.u; }
static inline float unbox_f32(u64 v) { if ((v >> 32) != 0xFFFFFFFF) return NAN; union { u32 u; float f; } c; c.u = (u32)v; return c.f; }
static inline u64 box_f64(double d) { union { double d; u64 u; } c; c.d = d; return c.u; }
static inline double unbox_f64(u64 v) { union { u64 u; double d; } c; c.u = v; return c.d; }
static void set_fp_rm(int rm, cpu_t *cpu) {
    if (rm == 7) rm = (cpu->csrs[CSR_FCSR] >> 5) & 7;
    switch (rm) {
        case 0: fesetround(FE_TONEAREST); break;
        case 1: fesetround(FE_TOWARDZERO); break;
        case 2: fesetround(FE_DOWNWARD); break;
        case 3: fesetround(FE_UPWARD); break;
        default: fesetround(FE_TONEAREST); break;
    }
}
static void update_fflags(cpu_t *cpu) {
    u8 flags = 0;
    int fe = fetestexcept(FE_ALL_EXCEPT);
    if (fe & FE_INEXACT)   flags |= FFLAG_NX;
    if (fe & FE_UNDERFLOW) flags |= FFLAG_UF;
    if (fe & FE_OVERFLOW)  flags |= FFLAG_OF;
    if (fe & FE_DIVBYZERO) flags |= FFLAG_DZ;
    if (fe & FE_INVALID)   flags |= FFLAG_NV;
    cpu->csrs[CSR_FCSR] |= flags;
}
static inline void mark_fs_dirty(cpu_t *cpu) {
    cpu->csrs[CSR_MSTATUS] |= (3ULL << 13); /* FS = Dirty */
}

/* Atomic reservation station */
static u64 reservation_addr = 0;
static bool reservation_valid = false;

#define TRAP(code, val) do { cpu->pc -= 4; cpu_take_trap(cpu, code, val, false); return; } while(0)

void cpu_execute(cpu_t *cpu, u32 inst) {
    static const void *dispatch_table[128] = {
        [0x37] = &&op_lui,   [0x17] = &&op_auipc, [0x6F] = &&op_jal,   [0x67] = &&op_jalr,
        [0x63] = &&op_br,    [0x03] = &&op_ld,    [0x23] = &&op_st,    [0x13] = &&op_imm,
        [0x1B] = &&op_imm32, [0x33] = &&op_op,    [0x3B] = &&op_op32,  [0x0F] = &&op_fence,
        [0x73] = &&op_sys,   [0x2F] = &&op_amo,   [0x07] = &&op_fld,   [0x27] = &&op_fst,
        [0x43] = &&op_fmadd, [0x47] = &&op_fmsub, [0x4B] = &&op_fnmsub, [0x4F] = &&op_fnmadd,
        [0x53] = &&op_fop
    };

    u32 opcode = OPCODE(inst);
    if (__builtin_expect(!dispatch_table[opcode], 0)) goto op_default;

    u32 rd = RD(inst), rs1 = RS1(inst), rs2 = RS2(inst);
    u32 funct3 = FUNCT3(inst), funct7 = FUNCT7(inst);
    u64 *x = cpu->regs;

    goto *dispatch_table[opcode];

    /* ===== LUI ===== */
    op_lui: x[rd] = (u64)IMM_U(inst); return;
    /* ===== AUIPC ===== */
    op_auipc: x[rd] = cpu->pc - 4 + (u64)IMM_U(inst); return;
    /* ===== JAL ===== */
    op_jal: x[rd] = cpu->pc; cpu->pc = cpu->pc - 4 + (u64)IMM_J(inst); return;
    /* ===== JALR ===== */
    op_jalr: { u64 t = cpu->pc; cpu->pc = (x[rs1] + (u64)IMM_I(inst)) & ~1ULL; x[rd] = t; return; }

    /* ===== BRANCH ===== */
    op_br: {
        i64 imm = IMM_B(inst);
        bool taken = false;
        switch (funct3) {
            case 0: taken = (x[rs1] == x[rs2]); break; /* BEQ */
            case 1: taken = (x[rs1] != x[rs2]); break; /* BNE */
            case 4: taken = ((i64)x[rs1] < (i64)x[rs2]); break; /* BLT */
            case 5: taken = ((i64)x[rs1] >= (i64)x[rs2]); break; /* BGE */
            case 6: taken = (x[rs1] < x[rs2]); break; /* BLTU */
            case 7: taken = (x[rs1] >= x[rs2]); break; /* BGEU */
            default: TRAP(EXC_ILLEGAL_INST, inst);
        }
        if (taken) cpu->pc = cpu->pc - 4 + (u64)imm;
        return;
    }

    /* ===== LOAD ===== */
    op_ld: {
        u64 addr = x[rs1] + (u64)IMM_I(inst);
        load_result_t r;
        switch (funct3) {
            case 0: r = cpu_load(cpu, addr, SIZE_BYTE); if(r.exception){TRAP(r.exc_code,r.exc_val);} x[rd]=(i64)(i8)r.value; break;
            case 1: r = cpu_load(cpu, addr, SIZE_HALF); if(r.exception){TRAP(r.exc_code,r.exc_val);} x[rd]=(i64)(i16)r.value; break;
            case 2: r = cpu_load(cpu, addr, SIZE_WORD); if(r.exception){TRAP(r.exc_code,r.exc_val);} x[rd]=(i64)(i32)r.value; break;
            case 3: r = cpu_load(cpu, addr, SIZE_DWORD); if(r.exception){TRAP(r.exc_code,r.exc_val);} x[rd]=r.value; break;
            case 4: r = cpu_load(cpu, addr, SIZE_BYTE); if(r.exception){TRAP(r.exc_code,r.exc_val);} x[rd]=(u64)(u8)r.value; break;
            case 5: r = cpu_load(cpu, addr, SIZE_HALF); if(r.exception){TRAP(r.exc_code,r.exc_val);} x[rd]=(u64)(u16)r.value; break;
            case 6: r = cpu_load(cpu, addr, SIZE_WORD); if(r.exception){TRAP(r.exc_code,r.exc_val);} x[rd]=(u64)(u32)r.value; break;
            default: TRAP(EXC_ILLEGAL_INST, inst);
        }
        return;
    }

    /* ===== STORE ===== */
    op_st: {
        u64 addr = x[rs1] + (u64)IMM_S(inst);
        store_result_t r;
        switch (funct3) {
            case 0: r = cpu_store(cpu, addr, x[rs2], SIZE_BYTE); break;
            case 1: r = cpu_store(cpu, addr, x[rs2], SIZE_HALF); break;
            case 2: r = cpu_store(cpu, addr, x[rs2], SIZE_WORD); break;
            case 3: r = cpu_store(cpu, addr, x[rs2], SIZE_DWORD); break;
            default: TRAP(EXC_ILLEGAL_INST, inst);
        }
        if (r.exception) TRAP(r.exc_code, r.exc_val);
        return;
    }

    /* ===== OP-IMM (ADDI, SLTI, ...) ===== */
    op_imm: {
        i64 imm = IMM_I(inst);
        u32 shamt = rs2 | ((funct7 & 1) << 5); /* 6-bit for RV64 */
        switch (funct3) {
            case 0: x[rd] = x[rs1] + (u64)imm; break; /* ADDI */
            case 1: x[rd] = x[rs1] << shamt; break; /* SLLI */
            case 2: x[rd] = ((i64)x[rs1] < imm) ? 1 : 0; break; /* SLTI */
            case 3: x[rd] = (x[rs1] < (u64)imm) ? 1 : 0; break; /* SLTIU */
            case 4: x[rd] = x[rs1] ^ (u64)imm; break; /* XORI */
            case 5: if (funct7 & 0x20) x[rd] = (u64)((i64)x[rs1] >> shamt); else x[rd] = x[rs1] >> shamt; break;
            case 6: x[rd] = x[rs1] | (u64)imm; break; /* ORI */
            case 7: x[rd] = x[rs1] & (u64)imm; break; /* ANDI */
        }
        return;
    }

    /* ===== OP-IMM-32 (ADDIW, SLLIW, SRLIW, SRAIW) ===== */
    op_imm32: {
        i64 imm = IMM_I(inst);
        u32 shamt = rs2;
        switch (funct3) {
            case 0: x[rd] = (i64)(i32)(x[rs1] + (u64)imm); break; /* ADDIW */
            case 1: x[rd] = (i64)(i32)((u32)x[rs1] << shamt); break; /* SLLIW */
            case 5: if (funct7 & 0x20) x[rd] = (i64)(i32)((i32)x[rs1] >> shamt); else x[rd] = (i64)(i32)((u32)x[rs1] >> shamt); break;
            default: TRAP(EXC_ILLEGAL_INST, inst);
        }
        return;
    }

    /* ===== OP (ADD, SUB, MUL, ...) ===== */
    op_op: {
        if (funct7 == 0x01) {
            /* M extension */
            switch (funct3) {
                case 0: x[rd] = x[rs1] * x[rs2]; break; /* MUL */
                case 1: x[rd] = (u64)((__int128)(i64)x[rs1] * (__int128)(i64)x[rs2] >> 64); break; /* MULH */
                case 2: x[rd] = (u64)((__int128)(i64)x[rs1] * (unsigned __int128)x[rs2] >> 64); break; /* MULHSU */
                case 3: x[rd] = (u64)((unsigned __int128)x[rs1] * (unsigned __int128)x[rs2] >> 64); break; /* MULHU */
                case 4: if (x[rs2] == 0) x[rd] = ~0ULL; else if ((i64)x[rs1] == INT64_MIN && (i64)x[rs2] == -1) x[rd] = x[rs1]; else x[rd] = (u64)((i64)x[rs1] / (i64)x[rs2]); break;
                case 5: x[rd] = x[rs2] == 0 ? ~0ULL : x[rs1] / x[rs2]; break;
                case 6: if (x[rs2] == 0) x[rd] = x[rs1]; else if ((i64)x[rs1] == INT64_MIN && (i64)x[rs2] == -1) x[rd] = 0; else x[rd] = (u64)((i64)x[rs1] % (i64)x[rs2]); break;
                case 7: x[rd] = x[rs2] == 0 ? x[rs1] : x[rs1] % x[rs2]; break;
            }
        } else {
            switch (funct3) {
                case 0: x[rd] = funct7 & 0x20 ? x[rs1] - x[rs2] : x[rs1] + x[rs2]; break;
                case 1: x[rd] = x[rs1] << (x[rs2] & 63); break;
                case 2: x[rd] = ((i64)x[rs1] < (i64)x[rs2]) ? 1 : 0; break;
                case 3: x[rd] = (x[rs1] < x[rs2]) ? 1 : 0; break;
                case 4: x[rd] = x[rs1] ^ x[rs2]; break;
                case 5: x[rd] = funct7 & 0x20 ? (u64)((i64)x[rs1] >> (x[rs2] & 63)) : x[rs1] >> (x[rs2] & 63); break;
                case 6: x[rd] = x[rs1] | x[rs2]; break;
                case 7: x[rd] = x[rs1] & x[rs2]; break;
            }
        }
        return;
    }

    /* ===== OP-32 (ADDW, SUBW, ...) ===== */
    op_op32: {
        if (funct7 == 0x01) {
            switch (funct3) {
                case 0: x[rd] = (i64)(i32)((i32)x[rs1] * (i32)x[rs2]); break;
                case 4: if ((i32)x[rs2] == 0) x[rd] = ~0ULL; else if ((i32)x[rs1] == INT32_MIN && (i32)x[rs2] == -1) x[rd] = (i64)(i32)x[rs1]; else x[rd] = (i64)(i32)((i32)x[rs1] / (i32)x[rs2]); break;
                case 5: x[rd] = (u32)x[rs2] == 0 ? ~0ULL : (u64)(i64)(i32)((u32)x[rs1] / (u32)x[rs2]); break;
                case 6: if ((i32)x[rs2] == 0) x[rd] = (i64)(i32)x[rs1]; else if ((i32)x[rs1] == INT32_MIN && (i32)x[rs2] == -1) x[rd] = 0; else x[rd] = (i64)(i32)((i32)x[rs1] % (i32)x[rs2]); break;
                case 7: x[rd] = (u32)x[rs2] == 0 ? (i64)(i32)x[rs1] : (i64)(i32)((u32)x[rs1] % (u32)x[rs2]); break;
                default: TRAP(EXC_ILLEGAL_INST, inst);
            }
        } else {
            switch (funct3) {
                case 0: x[rd] = funct7 & 0x20 ? (i64)(i32)((u32)x[rs1] - (u32)x[rs2]) : (i64)(i32)((u32)x[rs1] + (u32)x[rs2]); break;
                case 1: x[rd] = (i64)(i32)((u32)x[rs1] << (x[rs2] & 31)); break;
                case 5: x[rd] = funct7 & 0x20 ? (i64)(i32)((i32)x[rs1] >> (x[rs2] & 31)) : (i64)(i32)((u32)x[rs1] >> (x[rs2] & 31)); break;
                default: TRAP(EXC_ILLEGAL_INST, inst);
            }
        }
        return;
    }

    op_fence: return; /* FENCE/FENCE.I NOP */

    /* ===== SYSTEM (ECALL, EBREAK, CSR, MRET, SRET, WFI, SFENCE.VMA) ===== */
    op_sys: {
        u16 csr_addr = (inst >> 20) & 0xFFF;
        if (funct3 == 0) {
            switch (csr_addr) {
                case 0x000: /* ECALL */
                    TRAP(cpu->priv == PRIV_U ? EXC_ECALL_FROM_U :
                         cpu->priv == PRIV_S ? EXC_ECALL_FROM_S : EXC_ECALL_FROM_M, 0);
                case 0x001: /* EBREAK */
                    TRAP(EXC_BREAKPOINT, cpu->pc - 4);
                case 0x102: { /* SRET */
                    if (cpu->priv < PRIV_S) TRAP(EXC_ILLEGAL_INST, inst);
                    if (cpu->priv == PRIV_S && (cpu->csrs[CSR_MSTATUS] & MSTATUS_TSR)) TRAP(EXC_ILLEGAL_INST, inst);
                    u64 ms = cpu->csrs[CSR_MSTATUS];
                    cpu->priv = (ms & MSTATUS_SPP) ? PRIV_S : PRIV_U;
                    u64 spie = (ms >> 5) & 1;
                    ms = (ms & ~MSTATUS_SIE) | (spie << 1);
                    ms |= MSTATUS_SPIE;
                    ms &= ~MSTATUS_SPP; ms &= ~MSTATUS_MPRV;
                    cpu->csrs[CSR_MSTATUS] = ms;
                    cpu->pc = cpu->csrs[CSR_SEPC];
                    cpu_update_mmu_state(cpu);
                    return;
                }
                case 0x302: { /* MRET */
                    if (cpu->priv < PRIV_M) TRAP(EXC_ILLEGAL_INST, inst);
                    u64 ms = cpu->csrs[CSR_MSTATUS];
                    cpu->priv = (ms >> 11) & 3;
                    u64 mpie = (ms >> 7) & 1;
                    ms = (ms & ~MSTATUS_MIE) | (mpie << 3);
                    ms |= MSTATUS_MPIE;
                    ms = (ms & ~MSTATUS_MPP) | (PRIV_U << 11);
                    if (cpu->priv != PRIV_M) ms &= ~MSTATUS_MPRV;
                    cpu->csrs[CSR_MSTATUS] = ms;
                    cpu->pc = cpu->csrs[CSR_MEPC];
                    cpu_update_mmu_state(cpu);
                    return;
                }
                case 0x105: cpu->wfi = true; return; /* WFI */
                default:
                    if (funct7 == 0x09) { /* SFENCE.VMA */
                        if (cpu->priv == PRIV_U) TRAP(EXC_ILLEGAL_INST, inst);
                        if (cpu->priv == PRIV_S && (cpu->csrs[CSR_MSTATUS] & MSTATUS_TVM)) TRAP(EXC_ILLEGAL_INST, inst);
                        mmu_tlb_flush(cpu);
                    } else TRAP(EXC_ILLEGAL_INST, inst);
                    return;
            }
        } else {
            u64 old = csr_read(cpu, csr_addr);
            u64 val;
            switch (funct3) {
                case 1: val = x[rs1]; csr_write(cpu, csr_addr, val); x[rd] = old; break;
                case 2: val = old | x[rs1]; if (rs1) csr_write(cpu, csr_addr, val); x[rd] = old; break;
                case 3: val = old & ~x[rs1]; if (rs1) csr_write(cpu, csr_addr, val); x[rd] = old; break;
                case 5: val = rs1; csr_write(cpu, csr_addr, val); x[rd] = old; break;
                case 6: val = old | rs1; if (rs1) csr_write(cpu, csr_addr, val); x[rd] = old; break;
                case 7: val = old & ~(u64)rs1; if (rs1) csr_write(cpu, csr_addr, val); x[rd] = old; break;
                default: TRAP(EXC_ILLEGAL_INST, inst);
            }
        }
        return;
    }

    /* ===== AMO (A extension) ===== */
    op_amo: {
        u64 addr = x[rs1];
        if (funct3 == 2) { /* 32-bit AMO */
            u32 op = funct7 >> 2;
            if (op == 0x02) { /* LR.W */
                load_result_t r = cpu_load(cpu, addr, SIZE_WORD);
                if (r.exception) TRAP(r.exc_code, r.exc_val);
                x[rd] = (i64)(i32)r.value;
                reservation_addr = addr; reservation_valid = true;
            } else if (op == 0x03) { /* SC.W */
                if (reservation_valid && reservation_addr == addr) {
                    store_result_t r = cpu_store(cpu, addr, x[rs2], SIZE_WORD);
                    if (r.exception) TRAP(r.exc_code, r.exc_val);
                    x[rd] = 0;
                } else { x[rd] = 1; }
                reservation_valid = false;
            } else {
                load_result_t r = cpu_load(cpu, addr, SIZE_WORD);
                if (r.exception) TRAP(r.exc_code, r.exc_val);
                i32 t = (i32)r.value, s = (i32)x[rs2], res;
                switch (op) {
                    case 0x00: res = (i32)((u32)t + (u32)s); break;
                    case 0x01: res = s; break;
                    case 0x04: res = t ^ s; break;
                    case 0x08: res = t | s; break;
                    case 0x0C: res = t & s; break;
                    case 0x10: res = t < s ? t : s; break;
                    case 0x14: res = t > s ? t : s; break;
                    case 0x18: res = (u32)t < (u32)s ? t : s; break;
                    case 0x1C: res = (u32)t > (u32)s ? t : s; break;
                    default: TRAP(EXC_ILLEGAL_INST, inst);
                }
                store_result_t sr = cpu_store(cpu, addr, (u32)res, SIZE_WORD);
                if (sr.exception) TRAP(sr.exc_code, sr.exc_val);
                x[rd] = (i64)(i32)t;
            }
        } else if (funct3 == 3) { /* 64-bit AMO */
            u32 op = funct7 >> 2;
            if (op == 0x02) {
                load_result_t r = cpu_load(cpu, addr, SIZE_DWORD);
                if (r.exception) TRAP(r.exc_code, r.exc_val);
                x[rd] = r.value; reservation_addr = addr; reservation_valid = true;
            } else if (op == 0x03) {
                if (reservation_valid && reservation_addr == addr) {
                    store_result_t r = cpu_store(cpu, addr, x[rs2], SIZE_DWORD);
                    if (r.exception) TRAP(r.exc_code, r.exc_val);
                    x[rd] = 0;
                } else x[rd] = 1;
                reservation_valid = false;
            } else {
                load_result_t r = cpu_load(cpu, addr, SIZE_DWORD);
                if (r.exception) TRAP(r.exc_code, r.exc_val);
                i64 t = (i64)r.value, s = (i64)x[rs2], res;
                switch (op) {
                    case 0x00: res = t + s; break;
                    case 0x01: res = s; break;
                    case 0x04: res = t ^ s; break;
                    case 0x08: res = t | s; break;
                    case 0x0C: res = t & s; break;
                    case 0x10: res = t < s ? t : s; break;
                    case 0x14: res = t > s ? t : s; break;
                    case 0x18: res = (u64)t < (u64)s ? t : s; break;
                    case 0x1C: res = (u64)t > (u64)s ? t : s; break;
                    default: TRAP(EXC_ILLEGAL_INST, inst);
                }
                store_result_t sr = cpu_store(cpu, addr, (u64)res, SIZE_DWORD);
                if (sr.exception) TRAP(sr.exc_code, sr.exc_val);
                x[rd] = (u64)t;
            }
        } else TRAP(EXC_ILLEGAL_INST, inst);
        return;
    }

    op_fld: {
        u64 addr = x[rs1] + (u64)IMM_I(inst);
        if (funct3 == 2) { 
            load_result_t r = cpu_load(cpu, addr, SIZE_WORD); if (r.exception) TRAP(r.exc_code, r.exc_val);
            cpu->fregs[rd] = 0xFFFFFFFF00000000ULL | (u32)r.value;
        } else if (funct3 == 3) {
            load_result_t r = cpu_load(cpu, addr, SIZE_DWORD); if (r.exception) TRAP(r.exc_code, r.exc_val);
            cpu->fregs[rd] = r.value;
        } else TRAP(EXC_ILLEGAL_INST, inst);
        mark_fs_dirty(cpu); return;
    }
    op_fst: {
        u64 addr = x[rs1] + (u64)IMM_S(inst);
        store_result_t r;
        if (funct3 == 2) r = cpu_store(cpu, addr, (u32)cpu->fregs[rs2], SIZE_WORD);
        else if (funct3 == 3) r = cpu_store(cpu, addr, cpu->fregs[rs2], SIZE_DWORD);
        else TRAP(EXC_ILLEGAL_INST, inst);
        if (r.exception) TRAP(r.exc_code, r.exc_val);
        return;
    }
    op_fmadd: {
        u32 fmt = (inst >> 25) & 3, rs3 = (inst >> 27) & 0x1F; mark_fs_dirty(cpu); feclearexcept(FE_ALL_EXCEPT); set_fp_rm(funct3, cpu);
        if (fmt == 0) cpu->fregs[rd] = box_f32(fmaf(unbox_f32(cpu->fregs[rs1]), unbox_f32(cpu->fregs[rs2]), unbox_f32(cpu->fregs[rs3])));
        else cpu->fregs[rd] = box_f64(fma(unbox_f64(cpu->fregs[rs1]), unbox_f64(cpu->fregs[rs2]), unbox_f64(cpu->fregs[rs3])));
        update_fflags(cpu); return;
    }
    op_fmsub: {
        u32 fmt = (inst >> 25) & 3, rs3 = (inst >> 27) & 0x1F; mark_fs_dirty(cpu); feclearexcept(FE_ALL_EXCEPT); set_fp_rm(funct3, cpu);
        if (fmt == 0) cpu->fregs[rd] = box_f32(fmaf(unbox_f32(cpu->fregs[rs1]), unbox_f32(cpu->fregs[rs2]), -unbox_f32(cpu->fregs[rs3])));
        else cpu->fregs[rd] = box_f64(fma(unbox_f64(cpu->fregs[rs1]), unbox_f64(cpu->fregs[rs2]), -unbox_f64(cpu->fregs[rs3])));
        update_fflags(cpu); return;
    }
    op_fnmsub: {
        u32 fmt = (inst >> 25) & 3, rs3 = (inst >> 27) & 0x1F; mark_fs_dirty(cpu); feclearexcept(FE_ALL_EXCEPT); set_fp_rm(funct3, cpu);
        if (fmt == 0) cpu->fregs[rd] = box_f32(fmaf(-unbox_f32(cpu->fregs[rs1]), unbox_f32(cpu->fregs[rs2]), unbox_f32(cpu->fregs[rs3])));
        else cpu->fregs[rd] = box_f64(fma(-unbox_f64(cpu->fregs[rs1]), unbox_f64(cpu->fregs[rs2]), unbox_f64(cpu->fregs[rs3])));
        update_fflags(cpu); return;
    }
    op_fnmadd: {
        u32 fmt = (inst >> 25) & 3, rs3 = (inst >> 27) & 0x1F; mark_fs_dirty(cpu); feclearexcept(FE_ALL_EXCEPT); set_fp_rm(funct3, cpu);
        if (fmt == 0) cpu->fregs[rd] = box_f32(fmaf(-unbox_f32(cpu->fregs[rs1]), unbox_f32(cpu->fregs[rs2]), -unbox_f32(cpu->fregs[rs3])));
        else cpu->fregs[rd] = box_f64(fma(-unbox_f64(cpu->fregs[rs1]), unbox_f64(cpu->fregs[rs2]), -unbox_f64(cpu->fregs[rs3])));
        update_fflags(cpu); return;
    }
    op_fop: {
        mark_fs_dirty(cpu); feclearexcept(FE_ALL_EXCEPT); set_fp_rm(funct3, cpu);
        switch (funct7) {
            case 0x00: cpu->fregs[rd] = box_f32(unbox_f32(cpu->fregs[rs1]) + unbox_f32(cpu->fregs[rs2])); break;
            case 0x04: cpu->fregs[rd] = box_f32(unbox_f32(cpu->fregs[rs1]) - unbox_f32(cpu->fregs[rs2])); break;
            case 0x08: cpu->fregs[rd] = box_f32(unbox_f32(cpu->fregs[rs1]) * unbox_f32(cpu->fregs[rs2])); break;
            case 0x0C: cpu->fregs[rd] = box_f32(unbox_f32(cpu->fregs[rs1]) / unbox_f32(cpu->fregs[rs2])); break;
            case 0x2C: cpu->fregs[rd] = box_f32(sqrtf(unbox_f32(cpu->fregs[rs1]))); break;
            case 0x10: { u32 a = (u32)cpu->fregs[rs1], b = (u32)cpu->fregs[rs2], r; switch (funct3) { case 0: r = (a & 0x7FFFFFFF) | (b & 0x80000000); break; case 1: r = (a & 0x7FFFFFFF) | (~b & 0x80000000); break; case 2: r = (a & 0x7FFFFFFF) | ((a ^ b) & 0x80000000); break; default: r = a; break; } cpu->fregs[rd] = 0xFFFFFFFF00000000ULL | r; break; }
            case 0x14: { float a = unbox_f32(cpu->fregs[rs1]), b = unbox_f32(cpu->fregs[rs2]); if (funct3 == 0) cpu->fregs[rd] = box_f32(isnan(a) ? b : isnan(b) ? a : fminf(a, b)); else cpu->fregs[rd] = box_f32(isnan(a) ? b : isnan(b) ? a : fmaxf(a, b)); break; }
            case 0x50: { float a = unbox_f32(cpu->fregs[rs1]), b = unbox_f32(cpu->fregs[rs2]); switch (funct3) { case 0: x[rd] = (a <= b) ? 1 : 0; break; case 1: x[rd] = (a < b) ? 1 : 0; break; case 2: x[rd] = (a == b) ? 1 : 0; break; default: TRAP(EXC_ILLEGAL_INST, inst); } break; }
            case 0x60: { float a = unbox_f32(cpu->fregs[rs1]); switch (rs2) { case 0: x[rd] = (i64)(i32)(i64)a; break; case 1: x[rd] = (i64)(i32)(u32)a; break; case 2: x[rd] = (i64)a; break; case 3: x[rd] = (u64)a; break; } break; }
            case 0x68: { switch (rs2) { case 0: cpu->fregs[rd] = box_f32((float)(i32)x[rs1]); break; case 1: cpu->fregs[rd] = box_f32((float)(u32)x[rs1]); break; case 2: cpu->fregs[rd] = box_f32((float)(i64)x[rs1]); break; case 3: cpu->fregs[rd] = box_f32((float)x[rs1]); break; } break; }
            case 0x70: if (funct3 == 0) x[rd] = (i64)(i32)(u32)cpu->fregs[rs1]; else { float f = unbox_f32(cpu->fregs[rs1]); u32 bits = (u32)cpu->fregs[rs1], cls = 0; if (f == -INFINITY) cls = 1 << 0; else if (f == INFINITY) cls = 1 << 7; else if (isnan(f)) cls = (bits == 0x7FC00000) ? (1 << 9) : (1 << 8); else if (f == 0.0f) cls = (bits & 0x80000000) ? (1 << 3) : (1 << 4); else if (fpclassify(f) == FP_SUBNORMAL) cls = (f < 0) ? (1 << 2) : (1 << 5); else cls = (f < 0) ? (1 << 1) : (1 << 6); x[rd] = cls; } break;
            case 0x78: cpu->fregs[rd] = 0xFFFFFFFF00000000ULL | (u32)x[rs1]; break;
            case 0x01: cpu->fregs[rd] = box_f64(unbox_f64(cpu->fregs[rs1]) + unbox_f64(cpu->fregs[rs2])); break;
            case 0x05: cpu->fregs[rd] = box_f64(unbox_f64(cpu->fregs[rs1]) - unbox_f64(cpu->fregs[rs2])); break;
            case 0x09: cpu->fregs[rd] = box_f64(unbox_f64(cpu->fregs[rs1]) * unbox_f64(cpu->fregs[rs2])); break;
            case 0x0D: cpu->fregs[rd] = box_f64(unbox_f64(cpu->fregs[rs1]) / unbox_f64(cpu->fregs[rs2])); break;
            case 0x2D: cpu->fregs[rd] = box_f64(sqrt(unbox_f64(cpu->fregs[rs1]))); break;
            case 0x11: { u64 a = cpu->fregs[rs1], b = cpu->fregs[rs2]; switch (funct3) { case 0: cpu->fregs[rd] = (a & 0x7FFFFFFFFFFFFFFFULL) | (b & 0x8000000000000000ULL); break; case 1: cpu->fregs[rd] = (a & 0x7FFFFFFFFFFFFFFFULL) | (~b & 0x8000000000000000ULL); break; case 2: cpu->fregs[rd] = (a & 0x7FFFFFFFFFFFFFFFULL) | ((a ^ b) & 0x8000000000000000ULL); break; } break; }
            case 0x15: { double a = unbox_f64(cpu->fregs[rs1]), b = unbox_f64(cpu->fregs[rs2]); if (funct3 == 0) cpu->fregs[rd] = box_f64(isnan(a) ? b : isnan(b) ? a : fmin(a, b)); else cpu->fregs[rd] = box_f64(isnan(a) ? b : isnan(b) ? a : fmax(a, b)); break; }
            case 0x20: cpu->fregs[rd] = box_f32((float)unbox_f64(cpu->fregs[rs1])); break;
            case 0x21: cpu->fregs[rd] = box_f64((double)unbox_f32(cpu->fregs[rs1])); break;
            case 0x51: { double a = unbox_f64(cpu->fregs[rs1]), b = unbox_f64(cpu->fregs[rs2]); switch (funct3) { case 0: x[rd] = (a <= b) ? 1 : 0; break; case 1: x[rd] = (a < b) ? 1 : 0; break; case 2: x[rd] = (a == b) ? 1 : 0; break; default: TRAP(EXC_ILLEGAL_INST, inst); } break; }
            case 0x61: { double a = unbox_f64(cpu->fregs[rs1]); switch (rs2) { case 0: x[rd] = (i64)(i32)a; break; case 1: x[rd] = (i64)(i32)(u32)a; break; case 2: x[rd] = (i64)a; break; case 3: x[rd] = (u64)a; break; } break; }
            case 0x69: { switch (rs2) { case 0: cpu->fregs[rd] = box_f64((double)(i32)x[rs1]); break; case 1: cpu->fregs[rd] = box_f64((double)(u32)x[rs1]); break; case 2: cpu->fregs[rd] = box_f64((double)(i64)x[rs1]); break; case 3: cpu->fregs[rd] = box_f64((double)x[rs1]); break; } break; }
            case 0x71: if (funct3 == 0) x[rd] = cpu->fregs[rs1]; else { double f = unbox_f64(cpu->fregs[rs1]); u64 bits = cpu->fregs[rs1], cls = 0; if (f == -INFINITY) cls = 1 << 0; else if (f == INFINITY) cls = 1 << 7; else if (isnan(f)) cls = (bits == 0x7FF8000000000000ULL) ? (1 << 9) : (1 << 8); else if (f == 0.0) cls = (bits & 0x8000000000000000ULL) ? (1 << 3) : (1 << 4); else if (fpclassify(f) == FP_SUBNORMAL) cls = (f < 0) ? (1 << 2) : (1 << 5); else cls = (f < 0) ? (1 << 1) : (1 << 6); x[rd] = cls; } break;
            case 0x79: cpu->fregs[rd] = x[rs1]; break;
            default: TRAP(EXC_ILLEGAL_INST, inst);
        }
        update_fflags(cpu); return;
    }

    op_default:
        if (cpu->priv == PRIV_U && inst == 0x464c457f) {
            cpu->regs[17] = 139; /* a7 = __NR_rt_sigreturn */
            TRAP(EXC_ECALL_FROM_U, 0);
        }
        TRAP(EXC_ILLEGAL_INST, inst);
}
