#include "dtb.h"

/*
 * Minimal FDT (Flattened Device Tree) generator.
 * We build the DTB manually according to the devicetree spec.
 */

/* FDT tokens */
#define FDT_BEGIN_NODE  0x00000001
#define FDT_END_NODE    0x00000002
#define FDT_PROP        0x00000003
#define FDT_NOP         0x00000004
#define FDT_END         0x00000009

/* Helper: write a big-endian 32-bit value */
static void put_be32(u8 *p, u32 val) {
    p[0] = (val >> 24) & 0xFF;
    p[1] = (val >> 16) & 0xFF;
    p[2] = (val >>  8) & 0xFF;
    p[3] =  val        & 0xFF;
}

static void put_be64(u8 *p, u64 val) {
    put_be32(p, (u32)(val >> 32));
    put_be32(p + 4, (u32)val);
}

/* String table builder */
typedef struct {
    char *data;
    int   len;
    int   cap;
} strtab_t;

static void strtab_init(strtab_t *st) {
    st->cap = 512;
    st->data = (char *)calloc(1, st->cap);
    st->len = 0;
}

static int strtab_add(strtab_t *st, const char *s) {
    /* Check if already exists */
    for (int i = 0; i < st->len; ) {
        if (strcmp(st->data + i, s) == 0) return i;
        i += strlen(st->data + i) + 1;
    }
    int off = st->len;
    int slen = strlen(s) + 1;
    while (st->len + slen > st->cap) {
        st->cap *= 2;
        st->data = (char *)realloc(st->data, st->cap);
    }
    memcpy(st->data + st->len, s, slen);
    st->len += slen;
    return off;
}

/* Structure block builder */
typedef struct {
    u8  *data;
    int  len;
    int  cap;
} structblk_t;

static void sb_init(structblk_t *sb) {
    sb->cap = 4096;
    sb->data = (u8 *)calloc(1, sb->cap);
    sb->len = 0;
}

static void sb_ensure(structblk_t *sb, int need) {
    while (sb->len + need > sb->cap) {
        sb->cap *= 2;
        sb->data = (u8 *)realloc(sb->data, sb->cap);
    }
}

static void sb_u32(structblk_t *sb, u32 val) {
    sb_ensure(sb, 4);
    put_be32(sb->data + sb->len, val);
    sb->len += 4;
}

static void sb_begin_node(structblk_t *sb, const char *name) {
    sb_u32(sb, FDT_BEGIN_NODE);
    int nlen = strlen(name) + 1;
    int aligned = (nlen + 3) & ~3;
    sb_ensure(sb, aligned);
    memset(sb->data + sb->len, 0, aligned);
    memcpy(sb->data + sb->len, name, nlen);
    sb->len += aligned;
}

static void sb_end_node(structblk_t *sb) {
    sb_u32(sb, FDT_END_NODE);
}

static void sb_prop(structblk_t *sb, strtab_t *st, const char *name, const void *data, int len) {
    sb_u32(sb, FDT_PROP);
    sb_u32(sb, len);
    sb_u32(sb, strtab_add(st, name));
    int aligned = (len + 3) & ~3;
    sb_ensure(sb, aligned);
    memset(sb->data + sb->len, 0, aligned);
    if (data && len > 0) {
        memcpy(sb->data + sb->len, data, len);
    }
    sb->len += aligned;
}

static void sb_prop_u32(structblk_t *sb, strtab_t *st, const char *name, u32 val) {
    u8 tmp[4];
    put_be32(tmp, val);
    sb_prop(sb, st, name, tmp, 4);
}

static void sb_prop_u64(structblk_t *sb, strtab_t *st, const char *name, u64 val) {
    u8 tmp[8];
    put_be64(tmp, val);
    sb_prop(sb, st, name, tmp, 8);
}

static void sb_prop_str(structblk_t *sb, strtab_t *st, const char *name, const char *val) {
    sb_prop(sb, st, name, val, strlen(val) + 1);
}

static void sb_prop_empty(structblk_t *sb, strtab_t *st, const char *name) {
    sb_prop(sb, st, name, NULL, 0);
}

/* Prop with two u32 cells */
static void sb_prop_2u32(structblk_t *sb, strtab_t *st, const char *name, u32 a, u32 b) {
    u8 tmp[8];
    put_be32(tmp, a);
    put_be32(tmp + 4, b);
    sb_prop(sb, st, name, tmp, 8);
}

/* Prop with two u64 cells (reg for #address-cells=2, #size-cells=2) */
static void sb_prop_reg64(structblk_t *sb, strtab_t *st, u64 addr, u64 size) {
    u8 tmp[16];
    put_be64(tmp, addr);
    put_be64(tmp + 8, size);
    sb_prop(sb, st, "reg", tmp, 16);
}

int dtb_generate(u8 *buf, int buf_size, u64 mem_base, u64 mem_size, u64 initrd_start, u64 initrd_end) {
    strtab_t st;
    structblk_t sb;
    strtab_init(&st);
    sb_init(&sb);

    /* Root node */
    sb_begin_node(&sb, "");
    sb_prop_u32(&sb, &st, "#address-cells", 2);
    sb_prop_u32(&sb, &st, "#size-cells", 2);
    sb_prop_str(&sb, &st, "compatible", "riscv-virtio");
    sb_prop_str(&sb, &st, "model", "riscv-virtio,qemu");

    /* chosen */
    sb_begin_node(&sb, "chosen");
    if (initrd_start < initrd_end) {
        sb_prop_u64(&sb, &st, "linux,initrd-start", initrd_start);
        sb_prop_u64(&sb, &st, "linux,initrd-end", initrd_end);
        sb_prop_str(&sb, &st, "bootargs", "root=/dev/ram0 rw console=ttyS0 earlycon=uart8250,mmio,0x10000000 rdinit=/bin/sh loglevel=8 nosoftlockup nowatchdog");
    } else {
        sb_prop_str(&sb, &st, "bootargs", "root=/dev/vda rw console=ttyS0 earlycon=uart8250,mmio,0x10000000 init=/bin/sh loglevel=8 nosoftlockup nowatchdog devtmpfs.mount=1");
    }
    /* Add rng-seed (32 bytes of entropy) */
    {
        u8 seed[32];
        for (int i = 0; i < 32; i++) seed[i] = (u8)rand();
        sb_prop(&sb, &st, "rng-seed", seed, 32);
    }
    sb_prop_str(&sb, &st, "stdout-path", "/soc/uart@10000000");
    sb_end_node(&sb);

    /* cpus */
    sb_begin_node(&sb, "cpus");
    sb_prop_u32(&sb, &st, "#address-cells", 1);
    sb_prop_u32(&sb, &st, "#size-cells", 0);
    sb_prop_u32(&sb, &st, "timebase-frequency", 10000000); /* 10 MHz */

    sb_begin_node(&sb, "cpu@0");
    sb_prop_str(&sb, &st, "device_type", "cpu");
    sb_prop_u32(&sb, &st, "reg", 0);
    sb_prop_str(&sb, &st, "status", "okay");
    sb_prop_str(&sb, &st, "compatible", "riscv");
    sb_prop_str(&sb, &st, "riscv,isa", "rv64imafdc_zicsr_zifencei");
    sb_prop_str(&sb, &st, "mmu-type", "riscv,sv39");

    sb_begin_node(&sb, "interrupt-controller");
    sb_prop_u32(&sb, &st, "#interrupt-cells", 1);
    sb_prop_empty(&sb, &st, "interrupt-controller");
    sb_prop_str(&sb, &st, "compatible", "riscv,cpu-intc");
    sb_prop_u32(&sb, &st, "phandle", 1);
    sb_end_node(&sb); /* interrupt-controller */

    sb_end_node(&sb); /* cpu@0 */
    sb_end_node(&sb); /* cpus */

    /* memory */
    sb_begin_node(&sb, "memory@80000000");
    sb_prop_str(&sb, &st, "device_type", "memory");
    sb_prop_reg64(&sb, &st, mem_base, mem_size);
    sb_end_node(&sb);

    /* soc */
    sb_begin_node(&sb, "soc");
    sb_prop_u32(&sb, &st, "#address-cells", 2);
    sb_prop_u32(&sb, &st, "#size-cells", 2);
    sb_prop_str(&sb, &st, "compatible", "simple-bus");
    sb_prop_empty(&sb, &st, "ranges");

    /* CLINT */
    sb_begin_node(&sb, "clint@2000000");
    sb_prop_str(&sb, &st, "compatible", "riscv,clint0");
    sb_prop_reg64(&sb, &st, CLINT_BASE, CLINT_SIZE);
    {
        u8 ints[16];
        put_be32(ints,      1);  /* phandle of cpu intc */
        put_be32(ints + 4,  3);  /* M-mode software interrupt */
        put_be32(ints + 8,  1);  /* phandle of cpu intc */
        put_be32(ints + 12, 7);  /* M-mode timer interrupt */
        sb_prop(&sb, &st, "interrupts-extended", ints, 16);
    }
    sb_end_node(&sb);

    /* PLIC */
    sb_begin_node(&sb, "plic@c000000");
    sb_prop_str(&sb, &st, "compatible", "sifive,plic-1.0.0");
    sb_prop_u32(&sb, &st, "#interrupt-cells", 1);
    sb_prop_empty(&sb, &st, "interrupt-controller");
    sb_prop_reg64(&sb, &st, PLIC_BASE, PLIC_SIZE);
    sb_prop_u32(&sb, &st, "riscv,ndev", 31);
    sb_prop_u32(&sb, &st, "phandle", 2);
    {
        u8 ints[32];
        put_be32(ints,       1);  /* phandle of cpu intc */
        put_be32(ints +  4, 11);  /* M-mode external interrupt */
        put_be32(ints +  8,  1);  /* phandle of cpu intc */
        put_be32(ints + 12,  9);  /* S-mode external interrupt */
        /* Pad unused context */
        put_be32(ints + 16, 0xFFFFFFFF);
        put_be32(ints + 20, 0xFFFFFFFF);
        put_be32(ints + 24, 0xFFFFFFFF);
        put_be32(ints + 28, 0xFFFFFFFF);
        sb_prop(&sb, &st, "interrupts-extended", ints, 16);
    }
    sb_end_node(&sb);

    /* UART */
    sb_begin_node(&sb, "uart@10000000");
    sb_prop_str(&sb, &st, "compatible", "ns16550a");
    sb_prop_reg64(&sb, &st, UART_BASE, UART_SIZE);
    sb_prop_u32(&sb, &st, "clock-frequency", 3686400);
    {
        u8 ints[8];
        put_be32(ints, 2);    /* phandle of PLIC */
        put_be32(ints + 4, UART_IRQ);
        sb_prop(&sb, &st, "interrupts-extended", ints, 8);
    }
    sb_end_node(&sb);

    /* Virtio block device */
    sb_begin_node(&sb, "virtio_mmio@10001000");
    sb_prop_str(&sb, &st, "compatible", "virtio,mmio");
    sb_prop_reg64(&sb, &st, VIRTIO_BASE, VIRTIO_SIZE);
    {
        u8 ints[8];
        put_be32(ints, 2);    /* phandle of PLIC */
        put_be32(ints + 4, VIRTIO_IRQ);
        sb_prop(&sb, &st, "interrupts-extended", ints, 8);
    }
    sb_end_node(&sb);

    /* Virtio net device */
    sb_begin_node(&sb, "virtio_mmio@10002000");
    sb_prop_str(&sb, &st, "compatible", "virtio,mmio");
    sb_prop_reg64(&sb, &st, 0x10002000ULL, 0x1000);  /* VIRTNET_BASE, VIRTNET_SIZE */
    {
        u8 ints[8];
        put_be32(ints, 2);    /* phandle of PLIC */
        put_be32(ints + 4, 2); /* VIRTNET_IRQ */
        sb_prop(&sb, &st, "interrupts-extended", ints, 8);
    }
    sb_end_node(&sb);

    /* Virtio RNG device */
    sb_begin_node(&sb, "virtio_mmio@10003000");
    sb_prop_str(&sb, &st, "compatible", "virtio,mmio");
    sb_prop_reg64(&sb, &st, VIRTRNG_BASE, VIRTRNG_SIZE);
    {
        u8 ints[8];
        put_be32(ints, 2);    /* phandle of PLIC */
        put_be32(ints + 4, VIRTRNG_IRQ);
        sb_prop(&sb, &st, "interrupts-extended", ints, 8);
    }
    sb_end_node(&sb);

    /* SYSCON (test/reset device) */
    sb_begin_node(&sb, "test@100000");
    {
        /* compatible = "sifive,test1\0sifive,test0\0syscon\0" */
        const char compat[] = "sifive,test1\0sifive,test0\0syscon";
        sb_prop(&sb, &st, "compatible", compat, sizeof(compat));
    }
    sb_prop_reg64(&sb, &st, SYSCON_BASE, SYSCON_SIZE);
    sb_prop_u32(&sb, &st, "phandle", 3);
    sb_end_node(&sb);

    sb_end_node(&sb); /* soc */

    /* poweroff node (top-level, outside soc) */
    sb_begin_node(&sb, "poweroff");
    sb_prop_str(&sb, &st, "compatible", "syscon-poweroff");
    sb_prop_u32(&sb, &st, "regmap", 3); /* phandle of syscon */
    sb_prop_u32(&sb, &st, "offset", 0);
    sb_prop_u32(&sb, &st, "value", 0x5555);
    sb_end_node(&sb);

    /* reboot node (top-level, outside soc) */
    sb_begin_node(&sb, "reboot");
    sb_prop_str(&sb, &st, "compatible", "syscon-reboot");
    sb_prop_u32(&sb, &st, "regmap", 3); /* phandle of syscon */
    sb_prop_u32(&sb, &st, "offset", 0);
    sb_prop_u32(&sb, &st, "value", 0x7777);
    sb_end_node(&sb);

    sb_end_node(&sb); /* root */

    sb_u32(&sb, FDT_END);

    /* Now assemble the complete FDT */
    int struct_size = sb.len;
    int strings_size = st.len;

    /* FDT header is 40 bytes */
    int header_size = 40;
    /* Memory reservation block: (initrd_start, isize) + (0, 0) if initrd exists, else just (0,0) */
    int rsv_count = (initrd_start < initrd_end) ? 2 : 1;
    int rsv_size = rsv_count * 16;
    
    int off_struct = header_size + rsv_size;
    /* Align struct */
    off_struct = (off_struct + 7) & ~7;
    int off_strings = off_struct + struct_size;
    int total_size = off_strings + strings_size;
    total_size = (total_size + 7) & ~7;

    if (total_size > buf_size) {
        fprintf(stderr, "DTB too large: %d > %d\n", total_size, buf_size);
        free(st.data);
        free(sb.data);
        return -1;
    }

    memset(buf, 0, total_size);

    /* FDT header */
    put_be32(buf +  0, 0xD00DFEED);          /* magic */
    put_be32(buf +  4, total_size);           /* totalsize */
    put_be32(buf +  8, off_struct);           /* off_dt_struct */
    put_be32(buf + 12, off_strings);          /* off_dt_strings */
    put_be32(buf + 16, header_size);          /* off_mem_rsvmap */
    put_be32(buf + 20, 17);                   /* version */
    put_be32(buf + 24, 16);                   /* last_comp_version */
    put_be32(buf + 28, 0);                    /* boot_cpuid_phys */
    put_be32(buf + 32, strings_size);         /* size_dt_strings */
    put_be32(buf + 36, struct_size);          /* size_dt_struct */

    /* Memory reservation block */
    int rsv_off = header_size;
    if (initrd_start < initrd_end) {
        put_be64(buf + rsv_off, initrd_start);
        put_be64(buf + rsv_off + 8, initrd_end - initrd_start);
        rsv_off += 16;
    }
    /* Terminating entry (0, 0) */
    put_be64(buf + rsv_off, 0);
    put_be64(buf + rsv_off + 8, 0);

    /* Copy structure block */
    memcpy(buf + off_struct, sb.data, struct_size);

    /* Copy strings block */
    memcpy(buf + off_strings, st.data, strings_size);

    free(st.data);
    free(sb.data);

    return total_size;
}
