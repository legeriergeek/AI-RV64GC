#include "cpu.h"

/* C extension (RV64C) compressed instruction decoder */
/* Expands 16-bit compressed instructions into equivalent operations */

static inline u64 sext(u64 val, int bits) { u64 m = 1ULL << (bits - 1); return (val ^ m) - m; }

/* C-extension register mapping: c_reg -> x(8+c_reg) */
#define CR(n) (8 + (n))

#define TRAP_C(code, val) do { cpu->pc -= 2; cpu_take_trap(cpu, code, val, false); return; } while(0)

void cpu_execute_compressed(cpu_t *cpu, u16 inst) {
    u64 *x = cpu->regs;
    u32 op = inst & 3;
    u32 funct3 = (inst >> 13) & 7;

    switch (op) {
    case 0: /* Quadrant 0 */
        switch (funct3) {
        case 0: { /* C.ADDI4SPN */
            u32 rd = CR((inst >> 2) & 7);
            u32 imm = ((inst >> 6) & 1) << 2 | ((inst >> 5) & 1) << 3 |
                      ((inst >> 11) & 3) << 4 | ((inst >> 7) & 0xF) << 6;
            /* nzuimm[5:4|9:6|2|3] */
            imm = ((inst >> 5) & 1) << 3 | ((inst >> 6) & 1) << 2 |
                  ((inst >> 7) & 0xF) << 6 | ((inst >> 11) & 3) << 4;
            if (imm == 0) TRAP_C(EXC_ILLEGAL_INST, inst);
            x[rd] = x[2] + imm;
            break;
        }
        case 1: { /* C.FLD */
            u32 rd = CR((inst >> 2) & 7);
            u32 rs1 = CR((inst >> 7) & 7);
            u32 imm = ((inst >> 10) & 7) << 3 | ((inst >> 5) & 3) << 6;
            u64 addr = x[rs1] + imm;
            load_result_t r = cpu_load(cpu, addr, SIZE_DWORD);
            if (r.exception) TRAP_C(r.exc_code, r.exc_val);
            cpu->fregs[rd] = r.value;
            cpu->csrs[CSR_MSTATUS] |= (3ULL << 13);
            break;
        }
        case 2: { /* C.LW */
            u32 rd = CR((inst >> 2) & 7);
            u32 rs1 = CR((inst >> 7) & 7);
            u32 imm = ((inst >> 6) & 1) << 2 | ((inst >> 10) & 7) << 3 | ((inst >> 5) & 1) << 6;
            u64 addr = x[rs1] + imm;
            load_result_t r = cpu_load(cpu, addr, SIZE_WORD);
            if (r.exception) TRAP_C(r.exc_code, r.exc_val);
            x[rd] = (i64)(i32)r.value;
            break;
        }
        case 3: { /* C.LD */
            u32 rd = CR((inst >> 2) & 7);
            u32 rs1 = CR((inst >> 7) & 7);
            u32 imm = ((inst >> 10) & 7) << 3 | ((inst >> 5) & 3) << 6;
            u64 addr = x[rs1] + imm;
            load_result_t r = cpu_load(cpu, addr, SIZE_DWORD);
            if (r.exception) TRAP_C(r.exc_code, r.exc_val);
            x[rd] = r.value;
            break;
        }
        case 5: { /* C.FSD */
            u32 rs2 = CR((inst >> 2) & 7);
            u32 rs1 = CR((inst >> 7) & 7);
            u32 imm = ((inst >> 10) & 7) << 3 | ((inst >> 5) & 3) << 6;
            u64 addr = x[rs1] + imm;
            store_result_t r = cpu_store(cpu, addr, cpu->fregs[rs2], SIZE_DWORD);
            if (r.exception) TRAP_C(r.exc_code, r.exc_val);
            break;
        }
        case 6: { /* C.SW */
            u32 rs2 = CR((inst >> 2) & 7);
            u32 rs1 = CR((inst >> 7) & 7);
            u32 imm = ((inst >> 6) & 1) << 2 | ((inst >> 10) & 7) << 3 | ((inst >> 5) & 1) << 6;
            u64 addr = x[rs1] + imm;
            store_result_t r = cpu_store(cpu, addr, x[rs2], SIZE_WORD);
            if (r.exception) TRAP_C(r.exc_code, r.exc_val);
            break;
        }
        case 7: { /* C.SD */
            u32 rs2 = CR((inst >> 2) & 7);
            u32 rs1 = CR((inst >> 7) & 7);
            u32 imm = ((inst >> 10) & 7) << 3 | ((inst >> 5) & 3) << 6;
            u64 addr = x[rs1] + imm;
            store_result_t r = cpu_store(cpu, addr, x[rs2], SIZE_DWORD);
            if (r.exception) TRAP_C(r.exc_code, r.exc_val);
            break;
        }
        default: TRAP_C(EXC_ILLEGAL_INST, inst);
        }
        break;

    case 1: /* Quadrant 1 */
        switch (funct3) {
        case 0: { /* C.NOP / C.ADDI */
            u32 rd = (inst >> 7) & 0x1F;
            i64 imm = sext(((inst >> 12) & 1) << 5 | ((inst >> 2) & 0x1F), 6);
            if (rd) x[rd] = x[rd] + (u64)imm;
            break;
        }
        case 1: { /* C.ADDIW */
            u32 rd = (inst >> 7) & 0x1F;
            i64 imm = sext(((inst >> 12) & 1) << 5 | ((inst >> 2) & 0x1F), 6);
            if (rd) x[rd] = (i64)(i32)(x[rd] + (u64)imm);
            break;
        }
        case 2: { /* C.LI */
            u32 rd = (inst >> 7) & 0x1F;
            i64 imm = sext(((inst >> 12) & 1) << 5 | ((inst >> 2) & 0x1F), 6);
            x[rd] = (u64)imm;
            break;
        }
        case 3: { /* C.ADDI16SP or C.LUI */
            u32 rd = (inst >> 7) & 0x1F;
            if (rd == 2) { /* C.ADDI16SP */
                i64 imm = sext(((inst >> 12) & 1) << 9 | ((inst >> 6) & 1) << 4 |
                             ((inst >> 5) & 1) << 6 | ((inst >> 3) & 3) << 7 |
                             ((inst >> 2) & 1) << 5, 10);
                if (imm == 0) TRAP_C(EXC_ILLEGAL_INST, inst);
                x[2] = x[2] + (u64)imm;
            } else if (rd) { /* C.LUI */
                i64 imm = sext(((inst >> 12) & 1) << 17 | ((inst >> 2) & 0x1F) << 12, 18);
                x[rd] = (u64)imm;
            }
            break;
        }
        case 4: { /* ALU operations */
            u32 funct2 = (inst >> 10) & 3;
            u32 rd = CR((inst >> 7) & 7);
            switch (funct2) {
            case 0: { /* C.SRLI */
                u32 shamt = ((inst >> 12) & 1) << 5 | ((inst >> 2) & 0x1F);
                x[rd] = x[rd] >> shamt;
                break;
            }
            case 1: { /* C.SRAI */
                u32 shamt = ((inst >> 12) & 1) << 5 | ((inst >> 2) & 0x1F);
                x[rd] = (u64)((i64)x[rd] >> shamt);
                break;
            }
            case 2: { /* C.ANDI */
                i64 imm = sext(((inst >> 12) & 1) << 5 | ((inst >> 2) & 0x1F), 6);
                x[rd] = x[rd] & (u64)imm;
                break;
            }
            case 3: { /* C.SUB, C.XOR, C.OR, C.AND, C.SUBW, C.ADDW */
                u32 rs2 = CR((inst >> 2) & 7);
                u32 funct1 = (inst >> 12) & 1;
                u32 funct2b = (inst >> 5) & 3;
                if (funct1 == 0) {
                    switch (funct2b) {
                        case 0: x[rd] = x[rd] - x[rs2]; break; /* C.SUB */
                        case 1: x[rd] = x[rd] ^ x[rs2]; break; /* C.XOR */
                        case 2: x[rd] = x[rd] | x[rs2]; break; /* C.OR */
                        case 3: x[rd] = x[rd] & x[rs2]; break; /* C.AND */
                    }
                } else {
                    switch (funct2b) {
                        case 0: x[rd] = (i64)(i32)((u32)x[rd] - (u32)x[rs2]); break; /* C.SUBW */
                        case 1: x[rd] = (i64)(i32)((u32)x[rd] + (u32)x[rs2]); break; /* C.ADDW */
                        default: TRAP_C(EXC_ILLEGAL_INST, inst);
                    }
                }
                break;
            }
            }
            break;
        }
        case 5: { /* C.J */
            i64 imm = sext(((inst >> 12) & 1) << 11 | ((inst >> 11) & 1) << 4 |
                         ((inst >> 9) & 3) << 8 | ((inst >> 8) & 1) << 10 |
                         ((inst >> 7) & 1) << 6 | ((inst >> 6) & 1) << 7 |
                         ((inst >> 3) & 7) << 1 | ((inst >> 2) & 1) << 5, 12);
            cpu->pc = cpu->pc - 2 + (u64)imm;
            break;
        }
        case 6: { /* C.BEQZ */
            u32 rs1 = CR((inst >> 7) & 7);
            i64 imm = sext(((inst >> 12) & 1) << 8 | ((inst >> 10) & 3) << 3 |
                         ((inst >> 5) & 3) << 6 | ((inst >> 3) & 3) << 1 |
                         ((inst >> 2) & 1) << 5, 9);
            if (x[rs1] == 0) cpu->pc = cpu->pc - 2 + (u64)imm;
            break;
        }
        case 7: { /* C.BNEZ */
            u32 rs1 = CR((inst >> 7) & 7);
            i64 imm = sext(((inst >> 12) & 1) << 8 | ((inst >> 10) & 3) << 3 |
                         ((inst >> 5) & 3) << 6 | ((inst >> 3) & 3) << 1 |
                         ((inst >> 2) & 1) << 5, 9);
            if (x[rs1] != 0) cpu->pc = cpu->pc - 2 + (u64)imm;
            break;
        }
        }
        break;

    case 2: /* Quadrant 2 */
        switch (funct3) {
        case 0: { /* C.SLLI */
            u32 rd = (inst >> 7) & 0x1F;
            u32 shamt = ((inst >> 12) & 1) << 5 | ((inst >> 2) & 0x1F);
            if (rd) x[rd] = x[rd] << shamt;
            break;
        }
        case 1: { /* C.FLDSP */
            u32 rd = (inst >> 7) & 0x1F;
            u32 imm = ((inst >> 12) & 1) << 5 | ((inst >> 5) & 3) << 3 | ((inst >> 2) & 7) << 6;
            u64 addr = x[2] + imm;
            load_result_t r = cpu_load(cpu, addr, SIZE_DWORD);
            if (r.exception) TRAP_C(r.exc_code, r.exc_val);
            cpu->fregs[rd] = r.value;
            cpu->csrs[CSR_MSTATUS] |= (3ULL << 13);
            break;
        }
        case 2: { /* C.LWSP */
            u32 rd = (inst >> 7) & 0x1F;
            u32 imm = ((inst >> 12) & 1) << 5 | ((inst >> 4) & 7) << 2 | ((inst >> 2) & 3) << 6;
            u64 addr = x[2] + imm;
            load_result_t r = cpu_load(cpu, addr, SIZE_WORD);
            if (r.exception) TRAP_C(r.exc_code, r.exc_val);
            if (rd) x[rd] = (i64)(i32)r.value;
            break;
        }
        case 3: { /* C.LDSP */
            u32 rd = (inst >> 7) & 0x1F;
            u32 imm = ((inst >> 12) & 1) << 5 | ((inst >> 5) & 3) << 3 | ((inst >> 2) & 7) << 6;
            u64 addr = x[2] + imm;
            load_result_t r = cpu_load(cpu, addr, SIZE_DWORD);
            if (r.exception) TRAP_C(r.exc_code, r.exc_val);
            if (rd) x[rd] = r.value;
            break;
        }
        case 4: { /* C.JR / C.MV / C.EBREAK / C.JALR / C.ADD */
            u32 rd = (inst >> 7) & 0x1F;
            u32 rs2 = (inst >> 2) & 0x1F;
            u32 bit12 = (inst >> 12) & 1;
            if (bit12 == 0) {
                if (rs2 == 0) { /* C.JR */
                    cpu->pc = x[rd] & ~1ULL;
                } else { /* C.MV */
                    if (rd) x[rd] = x[rs2];
                }
            } else {
                if (rs2 == 0 && rd == 0) { /* C.EBREAK */
                    TRAP_C(EXC_BREAKPOINT, cpu->pc - 2);
                } else if (rs2 == 0) { /* C.JALR */
                    u64 t = cpu->pc;
                    cpu->pc = x[rd] & ~1ULL;
                    x[1] = t;
                } else { /* C.ADD */
                    if (rd) x[rd] = x[rd] + x[rs2];
                }
            }
            break;
        }
        case 5: { /* C.FSDSP */
            u32 rs2 = (inst >> 2) & 0x1F;
            u32 imm = ((inst >> 10) & 7) << 3 | ((inst >> 7) & 7) << 6;
            u64 addr = x[2] + imm;
            store_result_t r = cpu_store(cpu, addr, cpu->fregs[rs2], SIZE_DWORD);
            if (r.exception) TRAP_C(r.exc_code, r.exc_val);
            break;
        }
        case 6: { /* C.SWSP */
            u32 rs2 = (inst >> 2) & 0x1F;
            u32 imm = ((inst >> 9) & 0xF) << 2 | ((inst >> 7) & 3) << 6;
            u64 addr = x[2] + imm;
            store_result_t r = cpu_store(cpu, addr, x[rs2], SIZE_WORD);
            if (r.exception) TRAP_C(r.exc_code, r.exc_val);
            break;
        }
        case 7: { /* C.SDSP */
            u32 rs2 = (inst >> 2) & 0x1F;
            u32 imm = ((inst >> 10) & 7) << 3 | ((inst >> 7) & 7) << 6;
            u64 addr = x[2] + imm;
            store_result_t r = cpu_store(cpu, addr, x[rs2], SIZE_DWORD);
            if (r.exception) TRAP_C(r.exc_code, r.exc_val);
            break;
        }
        }
        break;

    default:
        TRAP_C(EXC_ILLEGAL_INST, inst);
    }

    x[0] = 0;
}
