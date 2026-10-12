/*
 * xemu XB Online helpers
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "xemu-xbonline.h"

#define SECTOR_SIZE 512
#define CONFIG_SECTORS 0x400 /* config area sits ahead of the first partition */
#define CONFIG_MAGIC 0x79132568
#define QCOW2_MAGIC 0x514649fb
#define QCOW2_OFFSET_MASK 0x00fffffffffffe00ULL
#define QCOW2_COMPRESSED (1ULL << 62)

typedef struct HddImage {
    FILE *f;
    bool qcow2;
    unsigned int cluster_bits;
    uint32_t l1_size;
    uint64_t *l1;
} HddImage;

static bool hdd_read_at(HddImage *img, uint64_t offset, void *buf, size_t len)
{
    return fseeko(img->f, offset, SEEK_SET) == 0 &&
           fread(buf, 1, len, img->f) == len;
}

static bool hdd_open(HddImage *img, const char *path)
{
    uint8_t hdr[48];

    memset(img, 0, sizeof(*img));
    img->f = qemu_fopen(path, "rb");
    if (!img->f) {
        return false;
    }
    if (!hdd_read_at(img, 0, hdr, sizeof(hdr))) {
        return false;
    }
    if (ldl_be_p(hdr) != QCOW2_MAGIC) {
        return true; /* raw image */
    }

    img->qcow2 = true;
    img->cluster_bits = ldl_be_p(hdr + 20);
    img->l1_size = ldl_be_p(hdr + 36);
    if (ldl_be_p(hdr + 32) || img->cluster_bits < 9 || img->cluster_bits > 21 ||
        img->l1_size == 0 || img->l1_size > 0x100000) {
        return false;
    }
    img->l1 = g_new(uint64_t, img->l1_size);
    return hdd_read_at(img, ldq_be_p(hdr + 40), img->l1,
                       img->l1_size * sizeof(uint64_t));
}

static void hdd_close(HddImage *img)
{
    if (img->f) {
        fclose(img->f);
    }
    g_free(img->l1);
}

/* Returns 1 if the sector was read, 0 if it is unallocated, -1 on error */
static int hdd_read_sector(HddImage *img, uint64_t sector, uint8_t *buf)
{
    uint64_t offset = sector * SECTOR_SIZE;

    if (img->qcow2) {
        uint64_t cluster = offset >> img->cluster_bits;
        uint64_t l2_entries = 1ULL << (img->cluster_bits - 3);
        uint64_t l1_index = cluster / l2_entries;
        uint64_t l2_offset, entry;

        if (l1_index >= img->l1_size) {
            return 0;
        }
        l2_offset = be64_to_cpu(img->l1[l1_index]) & QCOW2_OFFSET_MASK;
        if (!l2_offset) {
            return 0;
        }
        if (!hdd_read_at(img, l2_offset + 8 * (cluster % l2_entries), &entry,
                         sizeof(entry))) {
            return -1;
        }
        entry = be64_to_cpu(entry);
        if (entry & QCOW2_COMPRESSED) {
            return -1;
        }
        if (!(entry & QCOW2_OFFSET_MASK)) {
            return 0;
        }
        offset = (entry & QCOW2_OFFSET_MASK) +
                 (offset & ((1ULL << img->cluster_bits) - 1));
    }

    return hdd_read_at(img, offset, buf, SECTOR_SIZE) ? 1 : -1;
}

int xemu_xbonline_hdd_has_account(const char *hdd_path)
{
    HddImage img;
    int result = 0;

    if (!hdd_open(&img, hdd_path)) {
        hdd_close(&img);
        return -1;
    }

    for (uint64_t sector = 0; sector < CONFIG_SECTORS; sector++) {
        uint8_t buf[SECTOR_SIZE];
        int r = hdd_read_sector(&img, sector, buf);
        if (r < 0) {
            result = -1;
            break;
        }
        /* The machine account name is the console serial prefixed "SN." */
        if (r > 0 && ldl_le_p(buf) == CONFIG_MAGIC &&
            !memcmp(buf + 24, "SN.", 3)) {
            result = 1;
            break;
        }
    }

    hdd_close(&img);
    return result;
}

char *xemu_xbonline_pick_game(const char *games_dir)
{
    GDir *dir;
    const char *name;
    char *preferred = NULL, *only = NULL;
    int count = 0;

    if (!games_dir || !games_dir[0]) {
        return NULL;
    }
    dir = g_dir_open(games_dir, 0, NULL);
    if (!dir) {
        return NULL;
    }

    while ((name = g_dir_read_name(dir))) {
        char *lower = g_ascii_strdown(name, -1);
        if (g_str_has_suffix(lower, ".iso") ||
            g_str_has_suffix(lower, ".xiso")) {
            count++;
            if (!preferred && strstr(lower, "black") && strstr(lower, "arrow")) {
                preferred = g_build_filename(games_dir, name, NULL);
            }
            if (count == 1) {
                only = g_build_filename(games_dir, name, NULL);
            }
        }
        g_free(lower);
    }
    g_dir_close(dir);

    if (preferred) {
        g_free(only);
        return preferred;
    }
    if (count == 1) {
        return only;
    }
    g_free(only);
    return NULL;
}
