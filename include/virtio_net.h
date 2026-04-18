#ifndef VIRTIO_NET_H
#define VIRTIO_NET_H

#include "types.h"
#include <pthread.h>

/* Virtio MMIO register offsets (same layout as virtio-blk) */
#define VIRTNET_MAGIC_VALUE          0x000
#define VIRTNET_VERSION              0x004
#define VIRTNET_DEVICE_ID            0x008
#define VIRTNET_VENDOR_ID            0x00C
#define VIRTNET_DEVICE_FEATURES      0x010
#define VIRTNET_DEVICE_FEATURES_SEL  0x014
#define VIRTNET_DRIVER_FEATURES      0x020
#define VIRTNET_DRIVER_FEATURES_SEL  0x024
#define VIRTNET_GUEST_PAGE_SIZE      0x028
#define VIRTNET_QUEUE_SEL            0x030
#define VIRTNET_QUEUE_NUM_MAX        0x034
#define VIRTNET_QUEUE_NUM            0x038
#define VIRTNET_QUEUE_ALIGN          0x03C
#define VIRTNET_QUEUE_PFN            0x040
#define VIRTNET_QUEUE_NOTIFY         0x050
#define VIRTNET_INTERRUPT_STATUS     0x060
#define VIRTNET_INTERRUPT_ACK        0x064
#define VIRTNET_STATUS               0x070

/* Virtio-net feature bits */
#define VIRTIO_NET_F_MAC             (1U << 5)
#define VIRTIO_NET_F_STATUS          (1U << 16)

/* Virtio-net header (prepended to each packet) */
typedef struct {
    u8  flags;
    u8  gso_type;
    u16 hdr_len;
    u16 gso_size;
    u16 csum_start;
    u16 csum_offset;
} __attribute__((packed)) virtio_net_hdr_t;

#define VIRTIO_NET_HDR_SIZE 10

/* Queue indices */
#define VIRTNET_RXQ  0
#define VIRTNET_TXQ  1
#define VIRTNET_NUM_QUEUES 2

#define VIRTNET_QUEUE_SIZE 256

/* Memory map for virtio-net */
#define VIRTNET_BASE  0x10002000ULL
#define VIRTNET_SIZE  0x00001000ULL
#define VIRTNET_IRQ   2

/* RX packet buffer */
#define VIRTNET_MAX_PKT_SIZE 1520
#define VIRTNET_RX_BUF_SIZE  (VIRTNET_QUEUE_SIZE * VIRTNET_MAX_PKT_SIZE)

/* Forward declaration */
struct cpu;

typedef struct {
    /* MMIO registers */
    u32 magic;
    u32 version;
    u32 device_id;
    u32 vendor_id;
    u32 device_features;
    u32 device_features_sel;
    u32 driver_features;
    u32 driver_features_sel;
    u32 guest_page_size;
    u32 queue_sel;
    u32 queue_num[VIRTNET_NUM_QUEUES];
    u32 queue_align[VIRTNET_NUM_QUEUES];
    u32 queue_pfn[VIRTNET_NUM_QUEUES];
    u32 queue_notify;
    u32 interrupt_status;
    u32 status;

    /* MAC address */
    u8  mac[6];

    /* TAP file descriptor */
    int tap_fd;

    /* RX ring buffer */
    u8  rx_buf[VIRTNET_RX_BUF_SIZE];
    int rx_pkt_sizes[VIRTNET_QUEUE_SIZE]; /* size of each buffered packet */
    int rx_head;                          /* read index */
    int rx_tail;                          /* write index */
    int rx_count;                         /* number of buffered packets */

    /* Thread for async RX */
    pthread_t       rx_thread;
    pthread_mutex_t rx_lock;
    bool            running;
    bool            interrupting;

    /* Avail ring tracking per queue */
    u16 last_avail_idx[VIRTNET_NUM_QUEUES];

    /* Reference to cpu for DMA */
    struct cpu *cpu;
} virtio_net_t;

/* API */
void virtio_net_init(virtio_net_t *vnet, const char *tap_name);
void virtio_net_free(virtio_net_t *vnet);
u64  virtio_net_load(virtio_net_t *vnet, u64 offset, int size);
void virtio_net_store(virtio_net_t *vnet, u64 offset, u64 value, int size, struct cpu *cpu);
bool virtio_net_is_interrupting(virtio_net_t *vnet);
void virtio_net_poll(virtio_net_t *vnet, struct cpu *cpu);

#endif /* VIRTIO_NET_H */
