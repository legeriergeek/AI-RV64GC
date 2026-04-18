#ifndef TYPES_H
#define TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <fenv.h>
#include <float.h>

extern bool g_verbose;
extern bool g_mips_report;

/* ---------- basic register types ---------- */
typedef uint64_t u64;
typedef int64_t  i64;
typedef uint32_t u32;
typedef int32_t  i32;
typedef uint16_t u16;
typedef int16_t  i16;
typedef uint8_t  u8;
typedef int8_t   i8;

/* ---------- memory access sizes ---------- */
#define SIZE_BYTE   1
#define SIZE_HALF   2
#define SIZE_WORD   4
#define SIZE_DWORD  8

/* ---------- memory map ---------- */
#define ROM_BASE        0x00001000ULL
#define ROM_SIZE        0x00001000ULL  /* 4 KiB */
#define CLINT_BASE      0x02000000ULL
#define CLINT_SIZE      0x00010000ULL
#define PLIC_BASE       0x0C000000ULL
#define PLIC_SIZE       0x04000000ULL
#define UART_BASE       0x10000000ULL
#define UART_SIZE       0x00000100ULL
#define SYSCON_BASE     0x00100000ULL
#define SYSCON_SIZE     0x00001000ULL
#define VIRTIO_BASE     0x10001000ULL
#define VIRTIO_SIZE     0x00001000ULL
#define VIRTRNG_BASE    0x10003000ULL
#define VIRTRNG_SIZE    0x00001000ULL
#define DRAM_BASE       0x80000000ULL
#define DRAM_SIZE       (512ULL * 1024 * 1024) /* 512 MiB */

/* ---------- privilege levels ---------- */
#define PRIV_U  0
#define PRIV_S  1
#define PRIV_M  3

/* ---------- exception codes ---------- */
#define EXC_INST_ADDR_MISALIGN   0
#define EXC_INST_ACCESS_FAULT    1
#define EXC_ILLEGAL_INST         2
#define EXC_BREAKPOINT           3
#define EXC_LOAD_ADDR_MISALIGN   4
#define EXC_LOAD_ACCESS_FAULT    5
#define EXC_STORE_ADDR_MISALIGN  6
#define EXC_STORE_ACCESS_FAULT   7
#define EXC_ECALL_FROM_U         8
#define EXC_ECALL_FROM_S         9
#define EXC_ECALL_FROM_M         11
#define EXC_INST_PAGE_FAULT      12
#define EXC_LOAD_PAGE_FAULT      13
#define EXC_STORE_PAGE_FAULT     15

/* ---------- interrupt codes (bit 63 set in mcause) ---------- */
#define IRQ_S_SOFT    1
#define IRQ_M_SOFT    3
#define IRQ_S_TIMER   5
#define IRQ_M_TIMER   7
#define IRQ_S_EXT     9
#define IRQ_M_EXT     11

/* ---------- MIP / MIE bits ---------- */
#define MIP_SSIP  (1ULL << IRQ_S_SOFT)
#define MIP_MSIP  (1ULL << IRQ_M_SOFT)
#define MIP_STIP  (1ULL << IRQ_S_TIMER)
#define MIP_MTIP  (1ULL << IRQ_M_TIMER)
#define MIP_SEIP  (1ULL << IRQ_S_EXT)
#define MIP_MEIP  (1ULL << IRQ_M_EXT)

/* ---------- mstatus bits ---------- */
#define MSTATUS_SIE   (1ULL << 1)
#define MSTATUS_MIE   (1ULL << 3)
#define MSTATUS_SPIE  (1ULL << 5)
#define MSTATUS_UBE   (1ULL << 6)
#define MSTATUS_MPIE  (1ULL << 7)
#define MSTATUS_SPP   (1ULL << 8)
#define MSTATUS_MPP   (3ULL << 11)
#define MSTATUS_FS    (3ULL << 13)
#define MSTATUS_XS    (3ULL << 15)
#define MSTATUS_MPRV  (1ULL << 17)
#define MSTATUS_SUM   (1ULL << 18)
#define MSTATUS_MXR   (1ULL << 19)
#define MSTATUS_TVM   (1ULL << 20)
#define MSTATUS_TW    (1ULL << 21)
#define MSTATUS_TSR   (1ULL << 22)
#define MSTATUS_UXL   (3ULL << 32)
#define MSTATUS_SXL   (3ULL << 34)
#define MSTATUS_SD    (1ULL << 63)

/* FS field values */
#define FS_OFF     0
#define FS_INITIAL 1
#define FS_CLEAN   2
#define FS_DIRTY   3

/* ---------- CSR addresses ---------- */
/* User */
#define CSR_FFLAGS      0x001
#define CSR_FRM         0x002
#define CSR_FCSR        0x003
#define CSR_CYCLE       0xC00
#define CSR_TIME        0xC01
#define CSR_INSTRET     0xC02

/* Supervisor */
#define CSR_SSTATUS     0x100
#define CSR_SIE         0x104
#define CSR_STVEC       0x105
#define CSR_SCOUNTEREN  0x106
#define CSR_SSCRATCH    0x140
#define CSR_SEPC        0x141
#define CSR_SCAUSE      0x142
#define CSR_STVAL       0x143
#define CSR_SIP         0x144
#define CSR_SATP        0x180

/* Machine */
#define CSR_MSTATUS     0x300
#define CSR_MISA        0x301
#define CSR_MEDELEG     0x302
#define CSR_MIDELEG     0x303
#define CSR_MIE         0x304
#define CSR_MTVEC       0x305
#define CSR_MCOUNTEREN  0x306
#define CSR_MSCRATCH    0x340
#define CSR_MEPC        0x341
#define CSR_MCAUSE      0x342
#define CSR_MTVAL       0x343
#define CSR_MIP         0x344
#define CSR_PMPCFG0     0x3A0
#define CSR_PMPADDR0    0x3B0
#define CSR_MVENDORID   0xF11
#define CSR_MARCHID     0xF12
#define CSR_MIMPID      0xF13
#define CSR_MHARTID     0xF14

/* ---------- SATP modes ---------- */
#define SATP_MODE_BARE  0
#define SATP_MODE_SV39  8
#define SATP_MODE_SV48  9

/* ---------- page table bits ---------- */
#define PTE_V  (1ULL << 0)
#define PTE_R  (1ULL << 1)
#define PTE_W  (1ULL << 2)
#define PTE_X  (1ULL << 3)
#define PTE_U  (1ULL << 4)
#define PTE_G  (1ULL << 5)
#define PTE_A  (1ULL << 6)
#define PTE_D  (1ULL << 7)

/* page size */
#define PAGE_SIZE  4096
#define PAGE_SHIFT 12

/* ---------- result type for bus operations ---------- */
typedef struct {
    u64  value;
    bool exception;
    u64  exc_code;
    u64  exc_val;
} load_result_t;

typedef struct {
    bool exception;
    u64  exc_code;
    u64  exc_val;
} store_result_t;

/* ---------- PLIC sources ---------- */
#define UART_IRQ    10
#define VIRTIO_IRQ  1
#define VIRTRNG_IRQ 3

/* ---------- FCSR rounding modes ---------- */
#define FRM_RNE  0  /* Round to Nearest, ties to Even */
#define FRM_RTZ  1  /* Round towards Zero */
#define FRM_RDN  2  /* Round Down (towards -inf) */
#define FRM_RUP  3  /* Round Up (towards +inf) */
#define FRM_RMM  4  /* Round to Nearest, ties to Max Magnitude */
#define FRM_DYN  7  /* Dynamic (use frm register) */

/* FCSR exception flags */
#define FFLAG_NX  0x01  /* Inexact */
#define FFLAG_UF  0x02  /* Underflow */
#define FFLAG_OF  0x04  /* Overflow */
#define FFLAG_DZ  0x08  /* Divide by Zero */
#define FFLAG_NV  0x10  /* Invalid Operation */

#endif /* TYPES_H */
