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
#include "qemu/bswap.h"
#include "system/block-backend.h"
#include "system/reset.h"

#include "xemu-dashinstall.h"
#include "xemu-settings.h"
#include "xemu-xbonline.h"
#include "xemu-notifications.h"

#define HDD_NAME "ide0-hd0"

static int resets;
static bool reset_hooked;
static bool swap_pending;

static bool swap_dashboards(void);

static void on_reset(void *opaque)
{
    qatomic_inc(&resets);
    if (swap_pending) {
        swap_pending = false;
        swap_dashboards();
    }
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

/* --- Switching between the XBL Dashboard and the one it replaced --- */

/* System partition (C:) */
#define C_OFFSET 0x8CA80000ULL
#define C_SIZE 0x1F400000ULL
#define ENTRY_SIZE 64
#define NAME_MAX_LEN 42
#define ATTR_DIRECTORY 0x10

/* Where a file's directory entry is, by its name, in the root of C: */
typedef struct DashEntry {
    const char *name;
    int64_t offset;  /* of its 64-byte entry on the disk, 0 if not found */
    uint8_t entry[ENTRY_SIZE];
} DashEntry;

static bool find_entries(BlockBackend *blk, DashEntry *want, int n)
{
    uint8_t hdr[16];
    if (blk_pread(blk, C_OFFSET, sizeof(hdr), hdr, 0) < 0 ||
        memcmp(hdr, "FATX", 4)) {
        return false;
    }
    uint32_t cluster_size = ldl_le_p(hdr + 8) * 512;
    if (cluster_size < 512 || cluster_size > 128 * 1024) {
        return false;
    }
    uint32_t clusters = C_SIZE / cluster_size;
    bool wide = clusters >= 0xfff0;
    uint64_t fat = C_OFFSET + 0x1000;
    uint64_t data = fat + ((clusters * (wide ? 4ULL : 2ULL) + 0xfff) & ~0xfffULL);

    uint8_t *buf = g_malloc(cluster_size);
    uint32_t dir = 1;  /* the root directory */
    bool done = false;
    for (int hops = 0; dir && !done && hops < 256; hops++) {
        uint64_t at = data + (uint64_t)(dir - 1) * cluster_size;
        if (blk_pread(blk, at, cluster_size, buf, 0) < 0) {
            break;
        }
        for (uint32_t i = 0; i < cluster_size; i += ENTRY_SIZE) {
            const uint8_t *e = buf + i;
            if (e[0] == 0x00 || e[0] == 0xff) {
                done = true;  /* end of the directory */
                break;
            }
            if (e[0] > NAME_MAX_LEN || (e[1] & ATTR_DIRECTORY)) {
                continue;  /* deleted (0xe5) or a directory */
            }
            for (int k = 0; k < n; k++) {
                if (!want[k].offset && e[0] == strlen(want[k].name) &&
                    !g_ascii_strncasecmp((const char *)e + 2, want[k].name, e[0])) {
                    want[k].offset = at + i;
                    memcpy(want[k].entry, e, ENTRY_SIZE);
                }
            }
        }
        uint8_t next[4];
        if (blk_pread(blk, fat + (uint64_t)dir * (wide ? 4 : 2), wide ? 4 : 2,
                      next, 0) < 0) {
            break;
        }
        dir = wide ? ldl_le_p(next) : lduw_le_p(next);
        if (dir == 0 || dir >= (wide ? 0xfffffff0 : 0xfff0) || dir > clusters) {
            dir = 0;
        }
    }
    g_free(buf);
    return true;
}

/* A directory entry with another name; the rest (clusters, size, dates) kept */
static void rename_entry(uint8_t *e, const char *name)
{
    size_t len = strlen(name);
    e[0] = len;
    memset(e + 2, 0xff, NAME_MAX_LEN);
    memcpy(e + 2, name, len);
}

/* Runs while the console is stopped for its restart */
static bool swap_dashboards(void)
{
    BlockBackend *blk = hdd();
    const char *fail = NULL;
    DashEntry e[2] = { { XEMU_XBONLINE_DASH_FILE },
                       { XEMU_XBONLINE_DASH_BACKUP_FILE } };

    if (!blk) {
        fail = "no hard disk";
    } else {
        blk_drain(blk);
        if (!find_entries(blk, e, 2)) {
            fail = "C: could not be read";
        } else if (!e[0].offset || !e[1].offset) {
            fail = "the backup dashboard is missing";
        }
    }
    if (!fail) {
        rename_entry(e[0].entry, XEMU_XBONLINE_DASH_BACKUP_FILE);
        rename_entry(e[1].entry, XEMU_XBONLINE_DASH_FILE);
        if (blk_pwrite(blk, e[0].offset, ENTRY_SIZE, e[0].entry, 0) < 0 ||
            blk_pwrite(blk, e[1].offset, ENTRY_SIZE, e[1].entry, 0) < 0 ||
            blk_flush(blk) < 0) {
            fail = "writing C: failed";
        }
    }

    xemu_xbonline_probe_hdd(g_config.sys.files.hdd_path);
    int now = xemu_xbonline_main_dashboard();
    fprintf(stderr, "dash switch: %s, dashboard now %d, backup %d\n",
            fail ? fail : "swapped", now, xemu_xbonline_backup_dashboard());
    if (fail) {
        char *msg = g_strdup_printf("The dashboard was not switched (%s).", fail);
        xemu_queue_error_message(msg);
        g_free(msg);
        return false;
    }
    xemu_queue_notification(now == XBONLINE_DASH_XBL ?
                                "Switched to the XBL Dashboard" :
                                "Switched to the original dashboard. "
                                "The XBL Dashboard is kept as its backup.");
    return true;
}

void xemu_dashinstall_switch_dashboard(void)
{
    xemu_dashinstall_resets();  /* hooks the restart */
    swap_pending = true;
}
