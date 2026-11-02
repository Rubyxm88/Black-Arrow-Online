/*
 * xemu: one-time hard disk grow, 8 GiB -> 64 GiB with an empty F: drive
 *
 * xemu's stock hard disk image is 8 GiB. The BIOS we ship gives everything
 * past 0x1DD156000 to F:, so growing the image and writing an empty FATX
 * file system there gives players a ~60 GB F: drive. The image file itself
 * only grows as the Xbox writes to it.
 *
 * Runs before the hard disk is attached, and only on a qcow2 image that is
 * exactly 8 GiB with no F: file system yet, so it happens once.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qobject/qdict.h"
#include "qemu/bswap.h"
#include "block/block.h"
#include "system/block-backend.h"

#include "xemu-hddgrow.h"
#include "xemu-notifications.h"

#define GROW_FROM   (8ULL << 30)
#define GROW_TO     (64ULL << 30)
#define F_START     0x1DD156000ULL
#define F_CLUSTER   0x4000          /* 16 KiB, as on E: */

static bool write_f(BlockBackend *blk)
{
    uint64_t clusters = (GROW_TO - F_START) / F_CLUSTER;
    uint64_t fat_bytes = (clusters * 4 + 0xFFF) & ~0xFFFULL;
    uint64_t data = F_START + 0x1000 + fat_bytes;
    uint8_t sb[0x1000];
    uint8_t root[F_CLUSTER];
    uint32_t serial = g_random_int();

    /* Superblock: magic, volume id, sectors per cluster, root dir cluster */
    memset(sb, 0xFF, sizeof(sb));
    memcpy(sb, "FATX", 4);
    stl_le_p(sb + 4, serial);
    stl_le_p(sb + 8, F_CLUSTER / 512);
    stl_le_p(sb + 12, 1);
    stw_le_p(sb + 16, 0);

    /* FAT: all free, then entry 0 = media, entry 1 = root (end of chain) */
    uint8_t fat[8];
    stl_le_p(fat, 0xFFFFFFF8);
    stl_le_p(fat + 4, 0xFFFFFFFF);

    /* Root directory: empty (0xFF ends the list) */
    memset(root, 0xFF, sizeof(root));

    return blk_pwrite_zeroes(blk, F_START + 0x1000, fat_bytes, 0) >= 0 &&
           blk_pwrite(blk, F_START, sizeof(sb), sb, 0) >= 0 &&
           blk_pwrite(blk, F_START + 0x1000, sizeof(fat), fat, 0) >= 0 &&
           blk_pwrite(blk, data, sizeof(root), root, 0) >= 0 &&
           blk_flush(blk) >= 0;
}

void xemu_hdd_grow(const char *path)
{
    if (!path || !path[0] || getenv("XEMU_NO_HDD_GROW")) {
        return;
    }

    /* qcow2 only: a raw image would really take 64 GB on the PC */
    QDict *options = qdict_new();
    qdict_put_str(options, "driver", "qcow2");
    Error *err = NULL;
    BlockBackend *blk = blk_new_open(path, NULL, options,
                                     BDRV_O_RDWR | BDRV_O_RESIZE, &err);
    if (!blk) {
        error_free(err);
        return;
    }

    int64_t size = blk_getlength(blk);
    uint8_t magic[4] = { 0 };
    if (size != (int64_t)GROW_FROM ||
        blk_pread(blk, F_START, sizeof(magic), magic, 0) < 0 ||
        !memcmp(magic, "FATX", 4)) {
        /* Not a stock disk, or F: is already there */
        blk_unref(blk);
        return;
    }

    if (blk_truncate(blk, GROW_TO, false, PREALLOC_MODE_OFF, 0, &err) < 0) {
        fprintf(stderr, "hdd grow: resize failed: %s\n", error_get_pretty(err));
        error_free(err);
        blk_unref(blk);
        return;
    }
    bool ok = write_f(blk);
    blk_unref(blk);

    if (ok) {
        fprintf(stderr, "hdd grow: %s is now 64 GiB with an empty F: drive\n",
                path);
        xemu_queue_notification("Your console's hard disk is now 64 GB, "
                                "with a new F: drive");
    } else {
        fprintf(stderr, "hdd grow: writing F: failed\n");
    }
}
