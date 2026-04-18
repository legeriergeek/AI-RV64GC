#include "virtio.h"
#include "cpu.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

void virtio_init(virtio_t *virtio, const char *disk_path) {
    memset(virtio, 0, sizeof(virtio_t));
    virtio->magic = 0x74726976;   /* "virt" in little-endian */
    virtio->version = 1;          /* Legacy interface */
    virtio->device_id = 2;        /* Block device */
    virtio->vendor_id = 0x554D4551; /* "QEMU" */
    virtio->device_features = 0;
    virtio->guest_page_size = 0;
    virtio->queue_num = VIRTIO_QUEUE_SIZE;
    virtio->interrupting = false;
    virtio->fd = -1;

    if (disk_path) {
        virtio->fd = open(disk_path, O_RDWR);
        if (virtio->fd < 0) {
            fprintf(stderr, "Warning: Could not open disk image '%s'\n", disk_path);
        } else {
            struct stat st;
            fstat(virtio->fd, &st);
            virtio->disk_size = st.st_size;
        }
    }
}

void virtio_free(virtio_t *virtio) {
    if (virtio->fd >= 0) {
        close(virtio->fd);
        virtio->fd = -1;
    }
}

u64 virtio_load(virtio_t *virtio, u64 offset, int size) {
    /* Legacy virtio_mmio config space is often read byte-by-byte by Linux! */
    if (offset >= 0x100 && offset < 0x120) {
        u8 config_space[32];
        memset(config_space, 0, sizeof(config_space));
        u64 sectors = virtio->disk_size / 512;
        
        /* The capacity field is a little-endian 64-bit integer at offset 0 of config space */
        for (int i = 0; i < 8; i++) {
            config_space[i] = (sectors >> (i * 8)) & 0xFF;
        }

        u32 config_offset = offset - 0x100;
        u64 val = 0;
        /* If they read a word or dword that overlaps, dynamically stitch it */
        for (int i = 0; i < size && (config_offset + i) < sizeof(config_space); i++) {
            val |= (u64)config_space[config_offset + i] << (i * 8);
        }
        return val;
    }

    switch (offset) {
        case VIRTIO_MAGIC_VALUE:     return virtio->magic;
        case VIRTIO_VERSION:         return virtio->version;
        case VIRTIO_DEVICE_ID:       return virtio->fd >= 0 ? virtio->device_id : 0;
        case VIRTIO_VENDOR_ID:       return virtio->vendor_id;
        case VIRTIO_DEVICE_FEATURES: return virtio->device_features;
        case VIRTIO_QUEUE_NUM_MAX:   return VIRTIO_QUEUE_SIZE;
        case VIRTIO_QUEUE_PFN:       return virtio->queue_pfn;
        case VIRTIO_INTERRUPT_STATUS:return virtio->interrupt_status;
        case VIRTIO_STATUS:          return virtio->status;
        default:                     return 0;
    }
}

void virtio_store(virtio_t *virtio, u64 offset, u64 value, int size, struct cpu *cpu) {
    (void)size;
    switch (offset) {
        case VIRTIO_DEVICE_FEATURES_SEL:
            virtio->device_features_sel = (u32)value;
            break;
        case VIRTIO_DRIVER_FEATURES:
            virtio->driver_features = (u32)value;
            break;
        case VIRTIO_DRIVER_FEATURES_SEL:
            virtio->driver_features_sel = (u32)value;
            break;
        case VIRTIO_GUEST_PAGE_SIZE:
            virtio->guest_page_size = (u32)value;
            break;
        case VIRTIO_QUEUE_SEL:
            virtio->queue_sel = (u32)value;
            break;
        case VIRTIO_QUEUE_NUM:
            virtio->queue_num = (u32)value;
            break;
        case VIRTIO_QUEUE_ALIGN:
            virtio->queue_align = (u32)value;
            break;
        case VIRTIO_QUEUE_PFN:
            virtio->queue_pfn = (u32)value;
            break;
        case VIRTIO_QUEUE_NOTIFY:
            virtio->queue_notify = (u32)value;
            /* Trigger appropriate access based on device type */
            if (virtio->device_id == 2) {
                virtio_disk_access(virtio, cpu);
            } else if (virtio->device_id == 4) {
                virtio_rng_access(virtio, cpu);
            }
            break;
        case VIRTIO_INTERRUPT_ACK:
            virtio->interrupt_status &= ~(u32)value;
            break;
        case VIRTIO_STATUS:
            virtio->status = (u32)value;
            break;
        default:
            break;
    }
}

/* Virtqueue descriptor */
typedef struct {
    u64 addr;
    u32 len;
    u16 flags;
    u16 next;
} __attribute__((packed)) vring_desc_t;

/* Virtqueue available ring */
typedef struct {
    u16 flags;
    u16 idx;
    u16 ring[];
} __attribute__((packed)) vring_avail_t;

/* Virtqueue used element */
typedef struct {
    u32 id;
    u32 len;
} __attribute__((packed)) vring_used_elem_t;

/* Virtqueue used ring */
typedef struct {
    u16 flags;
    u16 idx;
    vring_used_elem_t ring[];
} __attribute__((packed)) vring_used_t;

/* Virtio block request header */
typedef struct {
    u32 type;
    u32 reserved;
    u64 sector;
} __attribute__((packed)) virtio_blk_req_t;

static u64 read_mem_u64(cpu_t *cpu, u64 paddr) {
    load_result_t r = bus_load(&cpu->bus, paddr, SIZE_DWORD);
    return r.value;
}

static u32 read_mem_u32(cpu_t *cpu, u64 paddr) {
    load_result_t r = bus_load(&cpu->bus, paddr, SIZE_WORD);
    return (u32)r.value;
}

static u16 read_mem_u16(cpu_t *cpu, u64 paddr) {
    load_result_t r = bus_load(&cpu->bus, paddr, SIZE_HALF);
    return (u16)r.value;
}

static u8 read_mem_u8(cpu_t *cpu, u64 paddr) {
    load_result_t r = bus_load(&cpu->bus, paddr, SIZE_BYTE);
    return (u8)r.value;
}

static void write_mem_u8(cpu_t *cpu, u64 paddr, u8 val) {
    bus_store(&cpu->bus, paddr, val, SIZE_BYTE, cpu);
}

static void write_mem_u16(cpu_t *cpu, u64 paddr, u16 val) {
    bus_store(&cpu->bus, paddr, val, SIZE_HALF, cpu);
}

static void write_mem_u32(cpu_t *cpu, u64 paddr, u32 val) {
    bus_store(&cpu->bus, paddr, val, SIZE_WORD, cpu);
}

static inline void* guest_paddr_to_host(cpu_t *cpu, u64 paddr) {
    if (paddr >= DRAM_BASE && paddr < DRAM_BASE + cpu->bus.dram.size) {
        return &cpu->bus.dram.mem[paddr - DRAM_BASE];
    }
    return NULL;
}


void virtio_disk_access(virtio_t *virtio, cpu_t *cpu) {
    if (virtio->fd < 0 || virtio->guest_page_size == 0 || !virtio->queue_pfn) return;

    u64 base = (u64)virtio->queue_pfn * virtio->guest_page_size;
    u64 desc_base = base;
    u64 avail_base = base + VIRTIO_QUEUE_SIZE * 16;
    u64 used_base = (avail_base + 6 + 2 * VIRTIO_QUEUE_SIZE);
    used_base = (used_base + virtio->guest_page_size - 1) & ~((u64)virtio->guest_page_size - 1);

    u16 avail_idx = read_mem_u16(cpu, avail_base + 2);

    while (virtio->last_avail_idx != avail_idx) {
        u16 desc_idx = read_mem_u16(cpu, avail_base + 4 + (virtio->last_avail_idx % VIRTIO_QUEUE_SIZE) * 2);
        u64 desc_addr = desc_base + (u64)desc_idx * 16;
        vring_desc_t *d = (vring_desc_t*)guest_paddr_to_host(cpu, desc_addr);
        if (!d) break;

        virtio_blk_req_t *req = (virtio_blk_req_t*)guest_paddr_to_host(cpu, d->addr);
        if (!req) break;

        struct { u64 addr; u32 len; } data[128];
        int num_data = 0;
        u32 total_len = 0;
        u64 status_addr = 0;
        u32 used_len = 0;

        u16 current_flags = d->flags;
        u16 next = d->next;

        while (current_flags & VRING_DESC_F_NEXT) {
            u64 daddr = desc_base + (u64)next * 16;
            vring_desc_t *dn = (vring_desc_t*)guest_paddr_to_host(cpu, daddr);
            if (!dn) break;

            if (!(dn->flags & VRING_DESC_F_NEXT)) {
                status_addr = dn->addr;
                break;
            }

            if (num_data < 128) {
                data[num_data].addr = dn->addr;
                data[num_data].len = dn->len;
                total_len += dn->len;
                num_data++;
            }
            current_flags = dn->flags;
            next = dn->next;
        }

        u64 disk_offset = req->sector * 512;
        if (req->type == VIRTIO_BLK_T_IN || req->type == VIRTIO_BLK_T_OUT) {
            u32 current_disk_offset = 0;
            for (int i = 0; i < num_data; i++) {
                u32 l = data[i].len;
                void *hp = guest_paddr_to_host(cpu, data[i].addr);
                
                if (req->type == VIRTIO_BLK_T_IN) {
                    if (hp) {
                        if (pread(virtio->fd, hp, l, disk_offset + current_disk_offset) != (ssize_t)l) {
                            if (status_addr) write_mem_u8(cpu, status_addr, 1);
                            goto next_avail;
                        }
                    } else {
                        u32 to_read = (l > sizeof(virtio->io_buffer)) ? sizeof(virtio->io_buffer) : l;
                        if (pread(virtio->fd, virtio->io_buffer, to_read, disk_offset + current_disk_offset) != (ssize_t)to_read) {
                            if (status_addr) write_mem_u8(cpu, status_addr, 1);
                            goto next_avail;
                        }
                        for (u32 j = 0; j < to_read; j++) write_mem_u8(cpu, data[i].addr+j, virtio->io_buffer[j]);
                    }
                } else { /* OUT */
                    if (hp) {
                        if (pwrite(virtio->fd, hp, l, disk_offset + current_disk_offset) != (ssize_t)l) {
                            if (status_addr) write_mem_u8(cpu, status_addr, 1);
                            goto next_avail;
                        }
                    } else {
                        u32 to_write = (l > sizeof(virtio->io_buffer)) ? sizeof(virtio->io_buffer) : l;
                        for (u32 j = 0; j < to_write; j++) virtio->io_buffer[j] = read_mem_u8(cpu, data[i].addr+j);
                        if (pwrite(virtio->fd, virtio->io_buffer, to_write, disk_offset + current_disk_offset) != (ssize_t)to_write) {
                            if (status_addr) write_mem_u8(cpu, status_addr, 1);
                            goto next_avail;
                        }
                    }
                }
                current_disk_offset += l;
            }
            if (status_addr) {
                write_mem_u8(cpu, status_addr, 0);
                used_len = (req->type == VIRTIO_BLK_T_IN) ? total_len + 1 : 1;
            }
        } else { /* Flush/Others */
            if (status_addr) {
                write_mem_u8(cpu, status_addr, 0);
                used_len = 1;
            }
        }

        u16 used_idx = read_mem_u16(cpu, used_base + 2);
        vring_used_t *used_ring = (vring_used_t*)guest_paddr_to_host(cpu, used_base);
        if (used_ring) {
            u16 u_idx = used_idx % VIRTIO_QUEUE_SIZE;
            used_ring->ring[u_idx].id = (u32)desc_idx;
            used_ring->ring[u_idx].len = used_len;
            used_ring->idx = used_idx + 1;
        }
        virtio->interrupt_status |= 1;
        virtio->interrupting = true;
    next_avail:
        virtio->last_avail_idx++;
    }
}

void virtio_rng_access(virtio_t *virtio, cpu_t *cpu) {
    if (!virtio->queue_pfn) return;

    u64 desc_base = (u64)virtio->queue_pfn * virtio->guest_page_size;
    u64 avail_base = desc_base + VIRTIO_QUEUE_SIZE * 16;
    u64 used_base = (avail_base + 6 + 2 * VIRTIO_QUEUE_SIZE);
    used_base = (used_base + virtio->guest_page_size - 1) & ~((u64)virtio->guest_page_size - 1);

    u16 avail_idx = read_mem_u16(cpu, avail_base + 2);

    while (virtio->last_avail_idx != avail_idx) {
        u16 desc_idx = read_mem_u16(cpu, avail_base + 4 + (virtio->last_avail_idx % VIRTIO_QUEUE_SIZE) * 2);
        u64 desc_addr = desc_base + (u64)desc_idx * 16;
        u64 addr = read_mem_u64(cpu, desc_addr);
        u32 len = read_mem_u32(cpu, desc_addr + 8);

        if (g_verbose) fprintf(stderr, "[virtio-rng] Providing %u bytes of entropy to 0x%llx\n", len, (unsigned long long)addr);

        /* Fill descriptor with random data */
        for (u32 i = 0; i < len; i++) {
            write_mem_u8(cpu, addr + i, (u8)rand());
        }

        /* Update used ring */
        u16 used_idx = read_mem_u16(cpu, used_base + 2);
        u64 used_elem = used_base + 4 + (used_idx % VIRTIO_QUEUE_SIZE) * 8;
        write_mem_u32(cpu, used_elem, (u32)desc_idx);
        write_mem_u32(cpu, used_elem + 4, len);
        write_mem_u16(cpu, used_base + 2, used_idx + 1);

        virtio->interrupt_status |= 1;
        virtio->interrupting = true;
        virtio->last_avail_idx++;
    }
}

bool virtio_is_interrupting(virtio_t *virtio) {
    bool r = virtio->interrupting;
    if (r) virtio->interrupting = false;
    return r;
}
