#include "virtio_net.h"
#include "cpu.h"
#include <sys/ioctl.h>
#include <net/if.h>
#include <linux/if_tun.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

/* ---- TAP interface setup ---- */
static int tap_open(const char *dev_name) {
    int fd = open("/dev/net/tun", O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "[virtio-net] Cannot open /dev/net/tun: %s\n", strerror(errno));
        return -1;
    }

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    ifr.ifr_flags = IFF_TAP | IFF_NO_PI; /* TAP device, no packet info header */
    if (dev_name && dev_name[0]) {
        strncpy(ifr.ifr_name, dev_name, IFNAMSIZ - 1);
    }

    if (ioctl(fd, TUNSETIFF, (void *)&ifr) < 0) {
        fprintf(stderr, "[virtio-net] TUNSETIFF failed: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    /* Set non-blocking for reads */
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    fprintf(stderr, "[virtio-net] Opened TAP device: %s (fd=%d)\n", ifr.ifr_name, fd);
    return fd;
}

/* ---- DMA memory access helpers ---- */
static u64 vnet_read_mem_u64(struct cpu *cpu, u64 paddr) {
    load_result_t r = bus_load(&cpu->bus, paddr, SIZE_DWORD);
    return r.value;
}

static u32 vnet_read_mem_u32(struct cpu *cpu, u64 paddr) {
    load_result_t r = bus_load(&cpu->bus, paddr, SIZE_WORD);
    return (u32)r.value;
}

static u16 vnet_read_mem_u16(struct cpu *cpu, u64 paddr) {
    load_result_t r = bus_load(&cpu->bus, paddr, SIZE_HALF);
    return (u16)r.value;
}

static u8 vnet_read_mem_u8(struct cpu *cpu, u64 paddr) {
    load_result_t r = bus_load(&cpu->bus, paddr, SIZE_BYTE);
    return (u8)r.value;
}

static void vnet_write_mem_u8(struct cpu *cpu, u64 paddr, u8 val) {
    bus_store(&cpu->bus, paddr, val, SIZE_BYTE, cpu);
}

static void vnet_write_mem_u16(struct cpu *cpu, u64 paddr, u16 val) {
    bus_store(&cpu->bus, paddr, val, SIZE_HALF, cpu);
}

static void vnet_write_mem_u32(struct cpu *cpu, u64 paddr, u32 val) {
    bus_store(&cpu->bus, paddr, val, SIZE_WORD, cpu);
}

/* ---- RX thread: reads packets from TAP into a ring buffer ---- */
static void *virtio_net_rx_thread(void *arg) {
    virtio_net_t *vnet = (virtio_net_t *)arg;
    u8 pkt[VIRTNET_MAX_PKT_SIZE];

    while (vnet->running) {
        ssize_t n = read(vnet->tap_fd, pkt, sizeof(pkt));
        if (n > 0) {
            pthread_mutex_lock(&vnet->rx_lock);
            if (vnet->rx_count < VIRTNET_QUEUE_SIZE) {
                /* Store packet in the ring buffer at tail position */
                int offset = vnet->rx_tail * VIRTNET_MAX_PKT_SIZE;
                if (offset + n <= VIRTNET_RX_BUF_SIZE) {
                    memcpy(vnet->rx_buf + offset, pkt, n);
                    vnet->rx_pkt_sizes[vnet->rx_tail] = (int)n;
                    vnet->rx_tail = (vnet->rx_tail + 1) % VIRTNET_QUEUE_SIZE;
                    vnet->rx_count++;
                }
            }
            /* else drop packet */
            pthread_mutex_unlock(&vnet->rx_lock);
        } else {
            /* No data or EAGAIN — sleep briefly */
            usleep(500);
        }
    }
    return NULL;
}

/* ---- Init / Free ---- */
void virtio_net_init(virtio_net_t *vnet, const char *tap_name) {
    memset(vnet, 0, sizeof(virtio_net_t));

    vnet->magic = 0x74726976;   /* "virt" */
    vnet->version = 1;          /* Legacy MMIO interface */
    vnet->device_id = 1;        /* Network device */
    vnet->vendor_id = 0x554D4551; /* "QEMU" */

    /* Features: MAC address, link status */
    vnet->device_features = VIRTIO_NET_F_MAC | VIRTIO_NET_F_STATUS;

    /* Default MAC address: 52:54:00:12:34:56 (standard QEMU MAC) */
    vnet->mac[0] = 0x52; vnet->mac[1] = 0x54; vnet->mac[2] = 0x00;
    vnet->mac[3] = 0x12; vnet->mac[4] = 0x34; vnet->mac[5] = 0x56;

    for (int i = 0; i < VIRTNET_NUM_QUEUES; i++) {
        vnet->queue_num[i] = VIRTNET_QUEUE_SIZE;
        vnet->queue_align[i] = 0;
        vnet->queue_pfn[i] = 0;
        vnet->last_avail_idx[i] = 0;
    }

    vnet->interrupting = false;
    vnet->running = false;
    vnet->tap_fd = -1;

    pthread_mutex_init(&vnet->rx_lock, NULL);

    /* Open TAP device */
    if (tap_name && tap_name[0]) {
        vnet->tap_fd = tap_open(tap_name);
        if (vnet->tap_fd >= 0) {
            vnet->running = true;
            pthread_create(&vnet->rx_thread, NULL, virtio_net_rx_thread, vnet);
        }
    }
}

void virtio_net_free(virtio_net_t *vnet) {
    if (vnet->running) {
        vnet->running = false;
        pthread_join(vnet->rx_thread, NULL);
    }
    if (vnet->tap_fd >= 0) {
        close(vnet->tap_fd);
        vnet->tap_fd = -1;
    }
    pthread_mutex_destroy(&vnet->rx_lock);
}

/* ---- MMIO Load ---- */
u64 virtio_net_load(virtio_net_t *vnet, u64 offset, int size) {
    (void)size;

    /* Config space starts at offset 0x100 for legacy MMIO */
    if (offset >= 0x100 && offset < 0x100 + 6) {
        /* MAC address bytes */
        return vnet->mac[offset - 0x100];
    }
    if (offset == 0x106) {
        /* Network status: link up */
        return (vnet->tap_fd >= 0) ? 1 : 0;
    }

    switch (offset) {
        case VIRTNET_MAGIC_VALUE:       return vnet->magic;
        case VIRTNET_VERSION:           return vnet->version;
        case VIRTNET_DEVICE_ID:         return (vnet->tap_fd >= 0) ? vnet->device_id : 0;
        case VIRTNET_VENDOR_ID:         return vnet->vendor_id;
        case VIRTNET_DEVICE_FEATURES:
            if (vnet->device_features_sel == 0)
                return vnet->device_features;
            return 0;
        case VIRTNET_QUEUE_NUM_MAX:     return VIRTNET_QUEUE_SIZE;
        case VIRTNET_QUEUE_PFN:
            if (vnet->queue_sel < VIRTNET_NUM_QUEUES)
                return vnet->queue_pfn[vnet->queue_sel];
            return 0;
        case VIRTNET_INTERRUPT_STATUS:  return vnet->interrupt_status;
        case VIRTNET_STATUS:            return vnet->status;
        default:                        return 0;
    }
}

/* ---- TX: transmit a packet from guest to TAP ---- */
static void virtio_net_tx(virtio_net_t *vnet, struct cpu *cpu) {
    if (vnet->tap_fd < 0 || vnet->guest_page_size == 0) return;
    int q = VIRTNET_TXQ;
    if (vnet->queue_pfn[q] == 0) return;

    u64 base = (u64)vnet->queue_pfn[q] * vnet->guest_page_size;
    u64 desc_base = base;
    u64 avail_base = base + VIRTNET_QUEUE_SIZE * 16;
    u64 used_base = (avail_base + 6 + 2 * VIRTNET_QUEUE_SIZE);
    used_base = (used_base + vnet->guest_page_size - 1) & ~((u64)vnet->guest_page_size - 1);

    u16 avail_idx = vnet_read_mem_u16(cpu, avail_base + 2);

    while (vnet->last_avail_idx[q] != avail_idx) {
        u16 desc_idx = vnet_read_mem_u16(cpu, avail_base + 4 + (vnet->last_avail_idx[q] % VIRTNET_QUEUE_SIZE) * 2);
        vnet->last_avail_idx[q]++;

        /* Walk descriptor chain, skip the virtio-net header, collect data */
        u8 pkt[VIRTNET_MAX_PKT_SIZE + VIRTIO_NET_HDR_SIZE];
        int pkt_len = 0;
        u16 idx = desc_idx;
        bool first = true;

        for (int safety = 0; safety < VIRTNET_QUEUE_SIZE; safety++) {
            u64 daddr = desc_base + (u64)idx * 16;
            u64 addr  = vnet_read_mem_u64(cpu, daddr);
            u32 len   = vnet_read_mem_u32(cpu, daddr + 8);
            u16 flags = vnet_read_mem_u16(cpu, daddr + 12);
            u16 next  = vnet_read_mem_u16(cpu, daddr + 14);

            int skip = 0;
            if (first) {
                skip = VIRTIO_NET_HDR_SIZE;
                first = false;
            }

            u32 to_read = len - skip;
            if (pkt_len + to_read > VIRTNET_MAX_PKT_SIZE) {
                to_read = VIRTNET_MAX_PKT_SIZE - pkt_len;
            }

            /* Fast path: if the buffer is entirely in DRAM, memcpy it */
            if (to_read > 0) {
                u64 p_addr = addr + skip;
                if (p_addr >= DRAM_BASE && p_addr + to_read <= DRAM_BASE + cpu->bus.dram.size) {
                    memcpy(pkt + pkt_len, cpu->bus.dram.mem + (p_addr - DRAM_BASE), to_read);
                    pkt_len += to_read;
                } else {
                    /* Slow path: MMIO byte-by-byte */
                    for (u32 i = skip; i < len && pkt_len < VIRTNET_MAX_PKT_SIZE; i++) {
                        pkt[pkt_len++] = vnet_read_mem_u8(cpu, addr + i);
                    }
                }
            }

            if (!(flags & 1)) break; /* VRING_DESC_F_NEXT */
            idx = next;
        }

        /* Write to TAP */
        if (pkt_len > 0) {
            if (write(vnet->tap_fd, pkt, pkt_len) < 0) {
                /* Ignore write errors */
            }
        }

        /* Update used ring */
        u16 used_idx = vnet_read_mem_u16(cpu, used_base + 2);
        u64 used_elem = used_base + 4 + (used_idx % VIRTNET_QUEUE_SIZE) * 8;
        vnet_write_mem_u32(cpu, used_elem, desc_idx);
        vnet_write_mem_u32(cpu, used_elem + 4, 0);
        vnet_write_mem_u16(cpu, used_base + 2, used_idx + 1);
    }

    vnet->interrupt_status |= 1;
    vnet->interrupting = true;
}

/* ---- RX: deliver buffered packets from TAP to guest ---- */
static void virtio_net_rx_deliver(virtio_net_t *vnet, struct cpu *cpu) {
    if (vnet->tap_fd < 0 || vnet->guest_page_size == 0) return;
    int q = VIRTNET_RXQ;
    if (vnet->queue_pfn[q] == 0) return;

    u64 base = (u64)vnet->queue_pfn[q] * vnet->guest_page_size;
    u64 desc_base = base;
    u64 avail_base = base + VIRTNET_QUEUE_SIZE * 16;
    u64 used_base = (avail_base + 6 + 2 * VIRTNET_QUEUE_SIZE);
    used_base = (used_base + vnet->guest_page_size - 1) & ~((u64)vnet->guest_page_size - 1);

    u16 avail_idx = vnet_read_mem_u16(cpu, avail_base + 2);

    pthread_mutex_lock(&vnet->rx_lock);

    while (vnet->rx_count > 0 && vnet->last_avail_idx[q] != avail_idx) {
        /* Get packet from ring buffer */
        int pkt_offset = vnet->rx_head * VIRTNET_MAX_PKT_SIZE;
        int pkt_len = vnet->rx_pkt_sizes[vnet->rx_head];

        /* Get descriptor from avail ring */
        u16 desc_idx = vnet_read_mem_u16(cpu, avail_base + 4 + (vnet->last_avail_idx[q] % VIRTNET_QUEUE_SIZE) * 2);
        vnet->last_avail_idx[q]++;

        /* Walk descriptor chain: write virtio-net header + packet data */
        u16 idx = desc_idx;
        int written = 0;
        bool header_written = false;

        for (int safety = 0; safety < VIRTNET_QUEUE_SIZE; safety++) {
            u64 daddr = desc_base + (u64)idx * 16;
            u64 addr  = vnet_read_mem_u64(cpu, daddr);
            u32 len   = vnet_read_mem_u32(cpu, daddr + 8);
            u16 flags = vnet_read_mem_u16(cpu, daddr + 12);
            u16 next  = vnet_read_mem_u16(cpu, daddr + 14);

            u32 buf_pos = 0;

            /* Write virtio-net header (all zeros = no offload) */
            if (!header_written) {
                if (addr >= DRAM_BASE && addr + VIRTIO_NET_HDR_SIZE <= DRAM_BASE + cpu->bus.dram.size && len >= VIRTIO_NET_HDR_SIZE) {
                    memset(cpu->bus.dram.mem + (addr - DRAM_BASE), 0, VIRTIO_NET_HDR_SIZE);
                    buf_pos += VIRTIO_NET_HDR_SIZE;
                } else {
                    for (u32 i = 0; i < VIRTIO_NET_HDR_SIZE && buf_pos < len; i++, buf_pos++) {
                        vnet_write_mem_u8(cpu, addr + buf_pos, 0);
                    }
                }
                header_written = true;
            }

            /* Write packet data */
            u32 to_write = len - buf_pos;
            if (written + to_write > pkt_len) {
                to_write = pkt_len - written;
            }
            if (to_write > 0) {
                u64 p_addr = addr + buf_pos;
                if (p_addr >= DRAM_BASE && p_addr + to_write <= DRAM_BASE + cpu->bus.dram.size) {
                    memcpy(cpu->bus.dram.mem + (p_addr - DRAM_BASE), vnet->rx_buf + pkt_offset + written, to_write);
                    buf_pos += to_write;
                    written += to_write;
                } else {
                    while (buf_pos < len && written < pkt_len) {
                        vnet_write_mem_u8(cpu, addr + buf_pos, vnet->rx_buf[pkt_offset + written]);
                        buf_pos++;
                        written++;
                    }
                }
            }

            if (!(flags & 1)) break; /* VRING_DESC_F_NEXT */
            idx = next;
        }

        /* Advance RX ring */
        vnet->rx_head = (vnet->rx_head + 1) % VIRTNET_QUEUE_SIZE;
        vnet->rx_count--;

        /* Update used ring */
        u16 used_idx = vnet_read_mem_u16(cpu, used_base + 2);
        u64 used_elem = used_base + 4 + (used_idx % VIRTNET_QUEUE_SIZE) * 8;
        vnet_write_mem_u32(cpu, used_elem, desc_idx);
        vnet_write_mem_u32(cpu, used_elem + 4, VIRTIO_NET_HDR_SIZE + written);
        vnet_write_mem_u16(cpu, used_base + 2, used_idx + 1);

        vnet->interrupt_status |= 1;
        vnet->interrupting = true;

        /* Re-read avail_idx in case more buffers were posted */
        avail_idx = vnet_read_mem_u16(cpu, avail_base + 2);
    }

    pthread_mutex_unlock(&vnet->rx_lock);
}

/* ---- MMIO Store ---- */
void virtio_net_store(virtio_net_t *vnet, u64 offset, u64 value, int size, struct cpu *cpu) {
    (void)size;
    switch (offset) {
        case VIRTNET_DEVICE_FEATURES_SEL:
            vnet->device_features_sel = (u32)value;
            break;
        case VIRTNET_DRIVER_FEATURES:
            vnet->driver_features = (u32)value;
            break;
        case VIRTNET_DRIVER_FEATURES_SEL:
            vnet->driver_features_sel = (u32)value;
            break;
        case VIRTNET_GUEST_PAGE_SIZE:
            vnet->guest_page_size = (u32)value;
            break;
        case VIRTNET_QUEUE_SEL:
            vnet->queue_sel = (u32)value;
            break;
        case VIRTNET_QUEUE_NUM:
            if (vnet->queue_sel < VIRTNET_NUM_QUEUES)
                vnet->queue_num[vnet->queue_sel] = (u32)value;
            break;
        case VIRTNET_QUEUE_ALIGN:
            if (vnet->queue_sel < VIRTNET_NUM_QUEUES)
                vnet->queue_align[vnet->queue_sel] = (u32)value;
            break;
        case VIRTNET_QUEUE_PFN:
            if (vnet->queue_sel < VIRTNET_NUM_QUEUES)
                vnet->queue_pfn[vnet->queue_sel] = (u32)value;
            break;
        case VIRTNET_QUEUE_NOTIFY:
            vnet->queue_notify = (u32)value;
            /* TX queue notification → transmit packets */
            if ((u32)value == VIRTNET_TXQ) {
                virtio_net_tx(vnet, cpu);
            }
            break;
        case VIRTNET_INTERRUPT_ACK:
            vnet->interrupt_status &= ~(u32)value;
            break;
        case VIRTNET_STATUS:
            vnet->status = (u32)value;
            if (value == 0) {
                /* Device reset */
                for (int i = 0; i < VIRTNET_NUM_QUEUES; i++) {
                    vnet->queue_pfn[i] = 0;
                    vnet->last_avail_idx[i] = 0;
                }
                vnet->interrupt_status = 0;
                vnet->interrupting = false;
            }
            break;
        default:
            break;
    }
}

/* ---- Interrupt check ---- */
bool virtio_net_is_interrupting(virtio_net_t *vnet) {
    bool r = vnet->interrupting;
    if (r) vnet->interrupting = false;
    return r;
}

/* ---- Poll: called periodically from device update to deliver RX packets ---- */
void virtio_net_poll(virtio_net_t *vnet, struct cpu *cpu) {
    if (vnet->tap_fd < 0) return;

    /* Check if there are packets to deliver */
    pthread_mutex_lock(&vnet->rx_lock);
    int count = vnet->rx_count;
    pthread_mutex_unlock(&vnet->rx_lock);

    if (count > 0) {
        virtio_net_rx_deliver(vnet, cpu);
    }
}
