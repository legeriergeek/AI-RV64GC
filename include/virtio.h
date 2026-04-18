#ifndef VIRTIO_H
#define VIRTIO_H

#include "types.h"

/* Virtio MMIO register offsets */
#define VIRTIO_MAGIC_VALUE       0x000
#define VIRTIO_VERSION           0x004
#define VIRTIO_DEVICE_ID         0x008
#define VIRTIO_VENDOR_ID         0x00C
#define VIRTIO_DEVICE_FEATURES   0x010
#define VIRTIO_DEVICE_FEATURES_SEL 0x014
#define VIRTIO_DRIVER_FEATURES   0x020
#define VIRTIO_DRIVER_FEATURES_SEL 0x024
#define VIRTIO_GUEST_PAGE_SIZE   0x028
#define VIRTIO_QUEUE_SEL         0x030
#define VIRTIO_QUEUE_NUM_MAX     0x034
#define VIRTIO_QUEUE_NUM         0x038
#define VIRTIO_QUEUE_ALIGN       0x03C
#define VIRTIO_QUEUE_PFN         0x040
#define VIRTIO_QUEUE_NOTIFY      0x050
#define VIRTIO_INTERRUPT_STATUS  0x060
#define VIRTIO_INTERRUPT_ACK     0x064
#define VIRTIO_STATUS            0x070

/* Virtio block request types */
#define VIRTIO_BLK_T_IN   0
#define VIRTIO_BLK_T_OUT  1

/* Virtio descriptor flags */
#define VRING_DESC_F_NEXT     1
#define VRING_DESC_F_WRITE    2

#define VIRTIO_QUEUE_SIZE  16

/* Forward declaration */
struct cpu;

typedef struct {
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
    u32 queue_num;
    u32 queue_align;
    u32 queue_pfn;
    u32 queue_notify;
    u32 interrupt_status;
    u32 status;
    int   fd;         /* File descriptor for pread/pwrite */
    u64   disk_size;
    bool  interrupting;
    u16   last_avail_idx;
    u8    io_buffer[65536]; /* Persistent I/O buffer to avoid malloc */
    struct cpu *cpu; /* stored reference for DMA access */
} virtio_t;

void virtio_init(virtio_t *virtio, const char *disk_path);
void virtio_free(virtio_t *virtio);
u64  virtio_load(virtio_t *virtio, u64 offset, int size);
void virtio_store(virtio_t *virtio, u64 offset, u64 value, int size, struct cpu *cpu);
void virtio_disk_access(virtio_t *virtio, struct cpu *cpu);
void virtio_rng_access(virtio_t *virtio, struct cpu *cpu);
bool virtio_is_interrupting(virtio_t *virtio);

#endif /* VIRTIO_H */
