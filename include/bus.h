#ifndef BUS_H
#define BUS_H

#include "types.h"
#include "dram.h"
#include "uart.h"
#include "clint.h"
#include "plic.h"
#include "virtio.h"
#include "virtio_net.h"

/* Forward declaration */
struct cpu;

typedef struct {
    dram_t   dram;
    uart_t   uart;
    clint_t  clint;
    plic_t       plic;
    virtio_t     virtio;
    virtio_net_t virtio_net;
    virtio_t     virtio_rng;
    u8           rom[ROM_SIZE];
} bus_t;

void          bus_init(bus_t *bus, const char *disk_path, const char *tap_name);
void          bus_free(bus_t *bus);
load_result_t  bus_load(bus_t *bus, u64 addr, int size);
store_result_t bus_store(bus_t *bus, u64 addr, u64 value, int size, struct cpu *cpu);

#endif /* BUS_H */
