#ifndef DTB_H
#define DTB_H

#include "types.h"

/*
 * Generate a device tree blob describing our machine.
 * Returns the size of the DTB. The DTB is written into `buf`.
 * `buf` must be at least 4096 bytes.
 */
int dtb_generate(u8 *buf, int buf_size, u64 mem_base, u64 mem_size, u64 initrd_start, u64 initrd_end);

#endif /* DTB_H */
