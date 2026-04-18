#include "cpu.h"
#include "dtb.h"
#include <getopt.h>
#include <time.h>

bool g_verbose = false;
bool g_mips_report = false;

static void usage(const char *prog) {
    fprintf(stderr, "Usage: %s [options] <kernel.bin>\n", prog);
    fprintf(stderr, "  -d, --disk <file>    Disk image for virtio block device\n");
    fprintf(stderr, "  -r, --initrd <file>  Initrd image\n");
    fprintf(stderr, "  -t, --tap <dev>      TAP network device name (e.g. tap0)\n");
    fprintf(stderr, "  -v, --verbose        Enable instruction trace logging\n");
    fprintf(stderr, "  -m, --mips           Enable MIPS/Performance reporting\n");
    fprintf(stderr, "  -h, --help           Show this help\n");
}

int main(int argc, char **argv) {
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);
    srand(time(NULL));

    const char *disk_path = NULL;
    const char *initrd_path = NULL;
    const char *tap_name = NULL;
    static struct option long_opts[] = {
        {"disk", required_argument, 0, 'd'},
        {"initrd", required_argument, 0, 'r'},
        {"tap", required_argument, 0, 't'},
        {"verbose", no_argument, 0, 'v'},
        {"mips", no_argument, 0, 'm'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "d:r:t:vmh", long_opts, NULL)) != -1) {
        switch (opt) {
            case 'd': disk_path = optarg; break;
            case 'r': initrd_path = optarg; break;
            case 't': tap_name = optarg; break;
            case 'v': g_verbose = true; break;
            case 'm': g_mips_report = true; break;
            case 'h': usage(argv[0]); return 0;
            default: usage(argv[0]); return 1;
        }
    }

    if (optind >= argc) {
        usage(argv[0]);
        return 1;
    }

    const char *kernel_path = argv[optind];

    /* Read kernel binary */
    FILE *f = fopen(kernel_path, "rb");
    if (!f) {
        fprintf(stderr, "Error: Cannot open '%s'\n", kernel_path);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    u8 *kernel = (u8 *)malloc(fsize);
    if (!kernel || fread(kernel, 1, fsize, f) != (size_t)fsize) {
        fprintf(stderr, "Error: Failed to read '%s'\n", kernel_path);
        fclose(f);
        return 1;
    }
    fclose(f);

    fprintf(stderr, "[rvemu] Loaded kernel: %s (%ld bytes)\n", kernel_path, fsize);

    /* Initialize CPU */
    fprintf(stderr, "[rvemu] Initializing CPU...\n");
    cpu_t *cpu = (cpu_t*)malloc(sizeof(cpu_t));
    if (!cpu) {
        fprintf(stderr, "Error: Failed to allocate CPU structure\n");
        free(kernel);
        return 1;
    }
    cpu_init(cpu, disk_path, tap_name);
    
    fprintf(stderr, "[rvemu] Loading kernel binary...\n");
    cpu_load_binary(cpu, kernel, fsize);
    free(kernel);

    /* Load initrd if provided */
    u64 initrd_start = 0, initrd_end = 0;
    if (initrd_path) {
        FILE *fi = fopen(initrd_path, "rb");
        if (!fi) {
            fprintf(stderr, "Error: Cannot open initrd '%s'\n", initrd_path);
        } else {
            fseek(fi, 0, SEEK_END);
            long isize = ftell(fi);
            fseek(fi, 0, SEEK_SET);
            u8 *idata = (u8*)malloc(isize);
            if (idata && fread(idata, 1, isize, fi) == (size_t)isize) {
                /* Place initrd at 128MB offset in DRAM */
                u64 iaddr = DRAM_BASE + 128 * 1024 * 1024;
                dram_load_binary(&cpu->bus.dram, idata, isize, iaddr - DRAM_BASE);
                initrd_start = iaddr;
                initrd_end = iaddr + isize;
                fprintf(stderr, "[rvemu] Loaded initrd: %s (%ld bytes) at 0x%llx\n",
                        initrd_path, isize, (unsigned long long)initrd_start);
            }
            free(idata);
            fclose(fi);
        }
    }

    /* Generate and place DTB */
    fprintf(stderr, "[rvemu] Generating DTB...\n");
    u8 dtb_buf[4096];
    int dtb_size = dtb_generate(dtb_buf, sizeof(dtb_buf), DRAM_BASE, DRAM_SIZE, initrd_start, initrd_end);
    if (dtb_size < 0) {
        fprintf(stderr, "Error: DTB generation failed\n");
        cpu_free(cpu);
        free(cpu);
        return 1;
    }

    /* Place DTB at end of DRAM (aligned) */
    u64 dtb_addr = DRAM_BASE + DRAM_SIZE - (u64)dtb_size;
    dtb_addr &= ~0xFULL; /* Align to 16 bytes */
    dram_load_binary(&cpu->bus.dram, dtb_buf, dtb_size, dtb_addr - DRAM_BASE);

    /* Also copy DTB into ROM area for potential use */
    if ((u64)dtb_size <= ROM_SIZE) {
        memcpy(cpu->bus.rom, dtb_buf, dtb_size);
    }

    /* Set initial registers per SBI convention */
    cpu->regs[10] = 0;           /* a0 = hart ID */
    cpu->regs[11] = dtb_addr;    /* a1 = DTB address */

    fprintf(stderr, "[rvemu] DTB placed at 0x%llx (%d bytes)\n",
            (unsigned long long)dtb_addr, dtb_size);
    fprintf(stderr, "[rvemu] Starting emulation at 0x%llx\n",
            (unsigned long long)cpu->pc);

    /* Run */
    cpu_loop(cpu);

    cpu_free(cpu);
    free(cpu);
    return 0;
}
