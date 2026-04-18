#include "bus.h"

void bus_init(bus_t *bus, const char *disk_path, const char *tap_name) {
    dram_init(&bus->dram, DRAM_SIZE);
    uart_init(&bus->uart);
    clint_init(&bus->clint);
    plic_init(&bus->plic);
    virtio_init(&bus->virtio, disk_path);
    virtio_net_init(&bus->virtio_net, tap_name);
    virtio_init(&bus->virtio_rng, NULL);
    bus->virtio_rng.device_id = 4; /* RNG device */
    memset(bus->rom, 0, ROM_SIZE);
}

void bus_free(bus_t *bus) {
    dram_free(&bus->dram);
    uart_free(&bus->uart);
    virtio_free(&bus->virtio);
    virtio_net_free(&bus->virtio_net);
    virtio_free(&bus->virtio_rng);
}

load_result_t bus_load(bus_t *bus, u64 addr, int size) {
    load_result_t res = { .value = 0, .exception = false, .exc_code = 0, .exc_val = 0 };

    /* ROM */
    if (addr >= ROM_BASE && addr < ROM_BASE + ROM_SIZE) {
        u64 offset = addr - ROM_BASE;
        u64 val = 0;
        for (int i = 0; i < size; i++) {
            val |= (u64)bus->rom[offset + i] << (i * 8);
        }
        res.value = val;
        return res;
    }

    /* SYSCON (sifive,test - poweroff/reboot) */
    if (addr >= SYSCON_BASE && addr < SYSCON_BASE + SYSCON_SIZE) {
        res.value = 0;
        return res;
    }

    /* CLINT */
    if (addr >= CLINT_BASE && addr < CLINT_BASE + CLINT_SIZE) {
        res.value = clint_load(&bus->clint, addr - CLINT_BASE, size);
        return res;
    }

    /* PLIC */
    if (addr >= PLIC_BASE && addr < PLIC_BASE + PLIC_SIZE) {
        res.value = plic_load(&bus->plic, addr - PLIC_BASE, size);
        return res;
    }

    /* UART */
    if (addr >= UART_BASE && addr < UART_BASE + UART_SIZE) {
        res.value = uart_load(&bus->uart, addr - UART_BASE, size);
        return res;
    }

    /* Virtio block */
    if (addr >= VIRTIO_BASE && addr < VIRTIO_BASE + VIRTIO_SIZE) {
        res.value = virtio_load(&bus->virtio, addr - VIRTIO_BASE, size);
        return res;
    }

    /* Virtio net */
    if (addr >= VIRTNET_BASE && addr < VIRTNET_BASE + VIRTNET_SIZE) {
        res.value = virtio_net_load(&bus->virtio_net, addr - VIRTNET_BASE, size);
        return res;
    }

    /* Virtio RNG */
    if (addr >= VIRTRNG_BASE && addr < VIRTRNG_BASE + VIRTRNG_SIZE) {
        res.value = virtio_load(&bus->virtio_rng, addr - VIRTRNG_BASE, size);
        return res;
    }

    /* DRAM */
    if (addr >= DRAM_BASE && addr < DRAM_BASE + DRAM_SIZE) {
        u64 offset = addr - DRAM_BASE;
        if (offset + size > bus->dram.size) {
            res.exception = true;
            res.exc_code = EXC_LOAD_ACCESS_FAULT;
            res.exc_val = addr;
            return res;
        }
        res.value = dram_load(&bus->dram, offset, size);
        return res;
    }

    /* Access fault */
    res.exception = true;
    res.exc_code = EXC_LOAD_ACCESS_FAULT;
    res.exc_val = addr;
    return res;
}

store_result_t bus_store(bus_t *bus, u64 addr, u64 value, int size, struct cpu *cpu) {
    store_result_t res = { .exception = false, .exc_code = 0, .exc_val = 0 };

    /* ROM is read-only — silently ignore stores */
    if (addr >= ROM_BASE && addr < ROM_BASE + ROM_SIZE) {
        return res;
    }

    /* SYSCON (sifive,test - poweroff/reboot) */
    if (addr >= SYSCON_BASE && addr < SYSCON_BASE + SYSCON_SIZE) {
        u32 val = (u32)value;
        if (val == 0x5555) {
            fprintf(stderr, "[rvemu] System poweroff requested.\n");
            exit(0);
        } else if (val == 0x7777) {
            fprintf(stderr, "[rvemu] System reboot requested (not supported, exiting).\n");
            exit(0);
        }
        return res;
    }

    /* CLINT */
    if (addr >= CLINT_BASE && addr < CLINT_BASE + CLINT_SIZE) {
        clint_store(&bus->clint, addr - CLINT_BASE, value, size);
        return res;
    }

    /* PLIC */
    if (addr >= PLIC_BASE && addr < PLIC_BASE + PLIC_SIZE) {
        plic_store(&bus->plic, addr - PLIC_BASE, value, size);
        return res;
    }

    /* UART */
    if (addr >= UART_BASE && addr < UART_BASE + UART_SIZE) {
        uart_store(&bus->uart, addr - UART_BASE, value, size);
        return res;
    }

    /* Virtio block */
    if (addr >= VIRTIO_BASE && addr < VIRTIO_BASE + VIRTIO_SIZE) {
        virtio_store(&bus->virtio, addr - VIRTIO_BASE, value, size, cpu);
        return res;
    }

    /* Virtio net */
    if (addr >= VIRTNET_BASE && addr < VIRTNET_BASE + VIRTNET_SIZE) {
        virtio_net_store(&bus->virtio_net, addr - VIRTNET_BASE, value, size, cpu);
        return res;
    }

    /* Virtio RNG */
    if (addr >= VIRTRNG_BASE && addr < VIRTRNG_BASE + VIRTRNG_SIZE) {
        virtio_store(&bus->virtio_rng, addr - VIRTRNG_BASE, value, size, cpu);
        return res;
    }

    /* DRAM */
    if (addr >= DRAM_BASE && addr < DRAM_BASE + DRAM_SIZE) {
        u64 offset = addr - DRAM_BASE;
        if (offset + size > bus->dram.size) {
            res.exception = true;
            res.exc_code = EXC_STORE_ACCESS_FAULT;
            res.exc_val = addr;
            return res;
        }
        dram_store(&bus->dram, offset, value, size);
        return res;
    }

    /* Access fault */
    res.exception = true;
    res.exc_code = EXC_STORE_ACCESS_FAULT;
    res.exc_val = addr;
    return res;
}
