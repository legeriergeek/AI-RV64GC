#include "mmu.h"
#include "cpu.h"

void mmu_tlb_flush(cpu_t *cpu) {
    memset(cpu->tlb, 0, sizeof(cpu->tlb));
}

mmu_result_t mmu_translate(cpu_t *cpu, u64 vaddr, access_type_t access) {
    u64 satp = cpu->csrs[CSR_SATP];
    u64 mode = satp >> 60;
    u64 mstatus = cpu->csrs[CSR_MSTATUS];
    u8 priv = cpu->priv;

    if (access != ACCESS_EXEC && (mstatus & MSTATUS_MPRV)) {
        priv = (mstatus >> 11) & 3;
    }

    if (priv == PRIV_M || mode == SATP_MODE_BARE) {
        u8 *hp = NULL;
        if (vaddr >= DRAM_BASE && vaddr < DRAM_BASE + DRAM_SIZE) {
            hp = &cpu->bus.dram.mem[vaddr - DRAM_BASE];
        }
        return (mmu_result_t){ .paddr = vaddr, .host_ptr = hp, .exception = false };
    }

    if (mode != SATP_MODE_SV39) {
        return (mmu_result_t){ .exception = true, .exc_code = (access == ACCESS_EXEC) ? EXC_INST_PAGE_FAULT : (access == ACCESS_LOAD) ? EXC_LOAD_PAGE_FAULT : EXC_STORE_PAGE_FAULT, .exc_val = vaddr };
    }

    /* TLB Lookup */
    u64 vpn_query = vaddr >> 12;
    u32 tlb_idx = vpn_query & (TLB_SIZE - 1);
    tlb_entry_t *entry = &cpu->tlb[tlb_idx];

    if (entry->valid && entry->vpn == vpn_query) {
        u32 pte = entry->flags;
        /* Permission check */
        bool fault = false;
        if (priv == PRIV_S) {
            if (pte & PTE_U) {
                if (access == ACCESS_EXEC || !(mstatus & MSTATUS_SUM)) fault = true;
            }
        } else if (priv == PRIV_U) {
            if (!(pte & PTE_U)) fault = true;
        }

        if (!fault) {
            switch (access) {
                case ACCESS_EXEC:  if (!(pte & PTE_X)) fault = true; break;
                case ACCESS_LOAD:  if (!(pte & PTE_R) && (!(mstatus & MSTATUS_MXR) || !(pte & PTE_X))) fault = true; break;
                case ACCESS_STORE: if (!(pte & PTE_R) || !(pte & PTE_W)) fault = true; break;
            }
        }

        /* If permission OK and Accessed/Dirty bits don't need update, return fast */
        if (!fault && (pte & PTE_A) && (access != ACCESS_STORE || (pte & PTE_D))) {
            u64 pa = (entry->ppn << 12) | (vaddr & 0xFFF);
            u8 *hp = NULL;
            if (pa >= DRAM_BASE && pa < DRAM_BASE + DRAM_SIZE) {
                hp = &cpu->bus.dram.mem[pa - DRAM_BASE];
            }
            return (mmu_result_t){ .paddr = pa, .host_ptr = hp, .exception = false };
        }
    }

    /* Slow path: Sv39 3-level page table walk */
    u64 ppn = satp & 0x00000FFFFFFFFFFFULL;
    u64 vpn[3];
    vpn[0] = (vaddr >> 12) & 0x1FF;
    vpn[1] = (vaddr >> 21) & 0x1FF;
    vpn[2] = (vaddr >> 30) & 0x1FF;

    u64 a = ppn * PAGE_SIZE;
    u64 pte;
    u64 pte_addr;
    int level;

    for (level = 2; level >= 0; level--) {
        pte_addr = a + vpn[level] * 8;
        load_result_t lr = bus_load(&cpu->bus, pte_addr, SIZE_DWORD);
        if (lr.exception) goto page_fault;
        pte = lr.value;

        if (!(pte & PTE_V) || (!(pte & PTE_R) && (pte & PTE_W))) goto page_fault;

        if (!(pte & PTE_A)) {
            pte |= PTE_A;
            bus_store(&cpu->bus, pte_addr, pte, SIZE_DWORD, cpu);
        }

        if ((pte & PTE_R) || (pte & PTE_X)) break; /* Leaf */
        if (level == 0) goto page_fault;
        a = ((pte >> 10) & 0x00000FFFFFFFFFFFULL) * PAGE_SIZE;
    }

    if (level < 0) goto page_fault;

    /* Leaf permission checks */
    if (priv == PRIV_S) {
        if (pte & PTE_U) {
            if (access == ACCESS_EXEC || !(mstatus & MSTATUS_SUM)) goto page_fault;
        }
    } else if (priv == PRIV_U) {
        if (!(pte & PTE_U)) goto page_fault;
    }

    switch (access) {
        case ACCESS_EXEC:  if (!(pte & PTE_X)) goto page_fault; break;
        case ACCESS_LOAD:  if (!(pte & PTE_R) && (!(mstatus & MSTATUS_MXR) || !(pte & PTE_X))) goto page_fault; break;
        case ACCESS_STORE: if (!(pte & PTE_R) || !(pte & PTE_W)) goto page_fault; break;
    }

    /* Superpage alignment check */
    u64 pte_ppn = (pte >> 10) & 0x00000FFFFFFFFFFFULL;
    if (level > 0) {
        if (level == 2 && (pte_ppn & 0x3FFFF)) goto page_fault;
        if (level == 1 && (pte_ppn & 0x1FF))   goto page_fault;
    }

    if (access == ACCESS_STORE && !(pte & PTE_D)) {
        pte |= PTE_D;
        bus_store(&cpu->bus, pte_addr, pte, SIZE_DWORD, cpu);
    }

    /* Fill TLB if it's a normal page (not a superpage for simplicity) */
    if (level == 0) {
        entry->vpn = vpn_query;
        entry->ppn = pte_ppn;
        entry->flags = (u32)pte;
        entry->valid = true;
    }

    u64 pa;
    if (level == 2)      pa = (pte_ppn << 12) | (vaddr & 0x3FFFFFFF);
    else if (level == 1) pa = (pte_ppn << 12) | (vaddr & 0x1FFFFF);
    else                 pa = (pte_ppn << 12) | (vaddr & 0xFFF);

    u8 *hp = NULL;
    if (pa >= DRAM_BASE && pa < DRAM_BASE + DRAM_SIZE) {
        hp = &cpu->bus.dram.mem[pa - DRAM_BASE];
    }

    return (mmu_result_t){ .paddr = pa, .host_ptr = hp, .exception = false };

page_fault:
    return (mmu_result_t){ .exception = true, .exc_code = (access == ACCESS_EXEC) ? EXC_INST_PAGE_FAULT : (access == ACCESS_LOAD) ? EXC_LOAD_PAGE_FAULT : EXC_STORE_PAGE_FAULT, .exc_val = vaddr };
}
