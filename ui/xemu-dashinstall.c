/*
 * xemu: one-time XBL Dashboard install, the parts that talk to QEMU
 * (the rest is in ui/xui/dash-install.cc)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qemu/atomic.h"
#include "block/accounting.h"
#include "system/block-backend.h"
#include "system/reset.h"

#include "xemu-dashinstall.h"

#define HDD_NAME "ide0-hd0"

static int resets;
static bool reset_hooked;

static void on_reset(void *opaque)
{
    qatomic_inc(&resets);
}

static BlockBackend *hdd(void)
{
    BlockBackend *blk = blk_by_name(HDD_NAME);
    return blk && blk_is_available(blk) ? blk : NULL;
}

uint64_t xemu_dashinstall_hdd_written(void)
{
    BlockBackend *blk = hdd();
    return blk ? blk_get_stats(blk)->nr_bytes[BLOCK_ACCT_WRITE] : 0;
}

void xemu_dashinstall_hdd_flush(void)
{
    BlockBackend *blk = hdd();
    if (blk) {
        blk_flush(blk);
    }
}

int xemu_dashinstall_resets(void)
{
    if (!reset_hooked) {
        reset_hooked = true;
        qemu_register_reset(on_reset, NULL);
    }
    return qatomic_read(&resets);
}
