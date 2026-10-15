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

/* Whether text appears in the bytes, which may hold zeros */
static bool has_bytes(const uint8_t *buf, size_t len, const char *text)
{
    size_t n = strlen(text);
    for (size_t i = 0; i + n <= len; i++) {
        if (!memcmp(buf + i, text, n)) {
            return true;
        }
    }
    return false;
}

int xemu_xbonline_hdd_account_names(const char *hdd_path, char *out,
                                    size_t out_len)
{
    HddImage img;
    GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
    int count;

    out[0] = 0;
    if (!hdd_open(&img, hdd_path)) {
        hdd_close(&img);
        g_ptr_array_free(names, true);
        return -1;
    }

    /* Account records sit in the first sectors of the config area */
    for (uint64_t sector = 0; sector < 0x40; sector++) {
        uint8_t buf[SECTOR_SIZE];
        char name[17];
        bool printable = true, seen = false;

        if (hdd_read_sector(&img, sector, buf) <= 0 ||
            ldl_le_p(buf) != CONFIG_MAGIC) {
            continue;
        }
        /* A gamertag, then its realm; the machine account is "SN." */
        memcpy(name, buf + 24, 16);
        name[16] = 0;
        if (!name[0] || !strncmp(name, "SN.", 3) ||
            !has_bytes(buf + 40, 64, "xbox.com")) {
            continue;
        }
        for (const char *c = name; *c; c++) {
            printable &= *c >= 0x20 && *c < 0x7f;
        }
        /* Each account is stored twice; list it once */
        for (guint i = 0; i < names->len; i++) {
            seen |= !strcmp(g_ptr_array_index(names, i), name);
        }
        if (printable && !seen) {
            g_ptr_array_add(names, g_strdup(name));
        }
    }
    hdd_close(&img);

    for (guint i = 0; i < names->len; i++) {
        if (i) {
            g_strlcat(out, ", ", out_len);
        }
        g_strlcat(out, g_ptr_array_index(names, i), out_len);
    }
    count = names->len;
    g_ptr_array_free(names, true);
    return count;
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

/* Data partition (E:) of the Xbox hard disk */
#define FATX_E_OFFSET 0xABE80000ULL
#define FATX_E_SIZE 0x1312D6000ULL
#define FATX_MAGIC 0x58544146 /* "FATX" */
#define FATX_ENTRY_SIZE 64
#define FATX_ATTR_DIRECTORY 0x10
#define FATX_MAX_CLUSTER (128 * 1024)

typedef struct Fatx {
    HddImage *img;
    uint64_t fat_offset, data_offset;
    uint32_t cluster_size, cluster_count;
    bool wide;
} Fatx;

/* Sectors the image has no data for read as zeroes */
static bool hdd_read_bytes(HddImage *img, uint64_t offset, uint8_t *buf,
                           size_t len)
{
    while (len) {
        uint8_t sector[SECTOR_SIZE];
        size_t skip = offset % SECTOR_SIZE;
        size_t n = MIN(len, SECTOR_SIZE - skip);
        int r = hdd_read_sector(img, offset / SECTOR_SIZE, sector);

        if (r < 0) {
            return false;
        }
        if (r == 0) {
            memset(sector, 0, sizeof(sector));
        }
        memcpy(buf, sector + skip, n);
        buf += n;
        offset += n;
        len -= n;
    }
    return true;
}

static bool fatx_open(Fatx *fs, HddImage *img)
{
    uint8_t hdr[16];
    uint64_t fat_size;

    if (!hdd_read_bytes(img, FATX_E_OFFSET, hdr, sizeof(hdr)) ||
        ldl_le_p(hdr) != FATX_MAGIC) {
        return false;
    }
    fs->img = img;
    fs->cluster_size = ldl_le_p(hdr + 8) * SECTOR_SIZE;
    if (fs->cluster_size < SECTOR_SIZE || fs->cluster_size > FATX_MAX_CLUSTER) {
        return false;
    }
    fs->cluster_count = FATX_E_SIZE / fs->cluster_size;
    fs->wide = fs->cluster_count >= 0xfff0;
    fat_size = (uint64_t)fs->cluster_count * (fs->wide ? 4 : 2);
    fat_size = (fat_size + 0xfff) & ~0xfffULL;
    fs->fat_offset = FATX_E_OFFSET + 0x1000;
    fs->data_offset = fs->fat_offset + fat_size;
    return true;
}

/* Next cluster of a chain, or 0 at its end */
static uint32_t fatx_next(Fatx *fs, uint32_t cluster)
{
    uint8_t entry[4];
    uint32_t next;

    if (!hdd_read_bytes(fs->img,
                        fs->fat_offset + (uint64_t)cluster * (fs->wide ? 4 : 2),
                        entry, fs->wide ? 4 : 2)) {
        return 0;
    }
    next = fs->wide ? ldl_le_p(entry) : lduw_le_p(entry);
    if (next == 0 || next >= (fs->wide ? 0xfffffff0 : 0xfff0) ||
        next > fs->cluster_count) {
        return 0;
    }
    return next;
}

/*
 * Find a name in a directory. Returns its first cluster, or 0 if there is no
 * such entry of the wanted kind.
 */
static uint32_t fatx_find(Fatx *fs, uint32_t dir_cluster, const char *name,
                          bool want_dir)
{
    uint8_t *data = g_malloc(fs->cluster_size);
    size_t name_len = strlen(name);
    uint32_t found = 0;
    bool done = false;

    /* A directory is at most 4096 entries long */
    for (int hops = 0; dir_cluster && !done && hops < 4096; hops++) {
        if (!hdd_read_bytes(fs->img,
                            fs->data_offset +
                            (uint64_t)(dir_cluster - 1) * fs->cluster_size,
                            data, fs->cluster_size)) {
            break;
        }
        for (uint32_t i = 0; i < fs->cluster_size; i += FATX_ENTRY_SIZE) {
            const uint8_t *e = data + i;
            if (e[0] == 0x00 || e[0] == 0xff) {
                done = true; /* end of the directory */
                break;
            }
            if (e[0] == name_len &&
                !g_ascii_strncasecmp((const char *)e + 2, name, name_len) &&
                !(e[1] & FATX_ATTR_DIRECTORY) == !want_dir) {
                found = ldl_le_p(e + 44);
                done = true;
                break;
            }
        }
        dir_cluster = fatx_next(fs, dir_cluster);
    }

    g_free(data);
    return found;
}

/* Number of subdirectories of a directory */
static int fatx_count_dirs(Fatx *fs, uint32_t dir_cluster)
{
    uint8_t *data = g_malloc(fs->cluster_size);
    int count = 0;
    bool done = false;

    for (int hops = 0; dir_cluster && !done && hops < 4096; hops++) {
        if (!hdd_read_bytes(fs->img,
                            fs->data_offset +
                            (uint64_t)(dir_cluster - 1) * fs->cluster_size,
                            data, fs->cluster_size)) {
            break;
        }
        for (uint32_t i = 0; i < fs->cluster_size; i += FATX_ENTRY_SIZE) {
            const uint8_t *e = data + i;
            if (e[0] == 0x00 || e[0] == 0xff) {
                done = true;
                break;
            }
            if (e[0] != 0xe5 && (e[1] & FATX_ATTR_DIRECTORY)) {
                count++;
            }
        }
        dir_cluster = fatx_next(fs, dir_cluster);
    }

    g_free(data);
    return count;
}

int xemu_xbonline_hdd_has_dashboard(const char *hdd_path)
{
    HddImage img;
    Fatx fs;
    int result = -1;

    if (hdd_open(&img, hdd_path) && fatx_open(&fs, &img)) {
        uint32_t dir = fatx_find(&fs, 1, "XBLDashboard", true);
        result = dir && fatx_find(&fs, dir, "default.xbe", false) ? 1 : 0;
    }
    hdd_close(&img);
    return result;
}

/* Version and title of an XBE from its first bytes; false if it is not one */
static bool xbe_certificate(const uint8_t *xbe, size_t len, XbOnlineXbe *out)
{
    uint32_t base, cert;

    if (len < 0x180 || memcmp(xbe, "XBEH", 4)) {
        return false;
    }
    base = ldl_le_p(xbe + 0x104);
    cert = ldl_le_p(xbe + 0x118) - base;
    if (cert > len - 0xb0) {
        return false;
    }
    out->title_id = ldl_le_p(xbe + cert + 8);
    out->version = ldl_le_p(xbe + cert + 0xac);
    for (int i = 0; i < 39; i++) {
        uint16_t c = lduw_le_p(xbe + cert + 12 + 2 * i);
        out->name[i] = c == 0 ? 0 : (c < 0x80 ? c : '?');
        if (!c) {
            break;
        }
    }
    out->name[39] = 0;
    return true;
}

int xemu_xbonline_hdd_title_update(const char *hdd_path, uint32_t title_id,
                                   XbOnlineXbe *xbe, int *content_count)
{
    HddImage img;
    Fatx fs;
    int result = -1;
    char tid[9];

    *content_count = 0;
    snprintf(tid, sizeof(tid), "%08X", title_id);
    if (hdd_open(&img, hdd_path) && fatx_open(&fs, &img)) {
        uint32_t tdata = fatx_find(&fs, 1, "TDATA", true);
        uint32_t title = tdata ? fatx_find(&fs, tdata, tid, true) : 0;
        uint32_t update = title ? fatx_find(&fs, title, "$u", true) : 0;
        uint32_t content = title ? fatx_find(&fs, title, "$c", true) : 0;
        uint32_t file = update ? fatx_find(&fs, update, "default.xbe", false)
                               : 0;

        result = 0;
        if (file) {
            uint8_t *data = g_malloc(fs.cluster_size);
            if (hdd_read_bytes(&img, fs.data_offset +
                               (uint64_t)(file - 1) * fs.cluster_size,
                               data, fs.cluster_size) &&
                xbe_certificate(data, fs.cluster_size, xbe)) {
                result = 1;
            }
            g_free(data);
        }
        if (content) {
            *content_count = fatx_count_dirs(&fs, content);
        }
    }
    hdd_close(&img);
    return result;
}

bool xemu_xbonline_disc_xbe(const char *iso_path, XbOnlineXbe *xbe)
{
    static const uint64_t bases[] = { 0, 0x18300000ULL, 0x1040000ULL,
                                      0xFD90000ULL };
    FILE *f = qemu_fopen(iso_path, "rb");
    bool found = false;
    uint8_t vd[28];

    if (!f) {
        return false;
    }
    for (int b = 0; b < ARRAY_SIZE(bases) && !found; b++) {
        uint64_t base = bases[b];
        uint32_t root, root_size;
        uint8_t *dir;

        if (fseeko(f, base + 32 * 2048, SEEK_SET) ||
            fread(vd, 1, sizeof(vd), f) != sizeof(vd) ||
            memcmp(vd, "MICROSOFT*XBOX*MEDIA", 20)) {
            continue;
        }
        root = ldl_le_p(vd + 20);
        root_size = ldl_le_p(vd + 24);
        if (root_size == 0 || root_size > 1024 * 1024) {
            break;
        }
        dir = g_malloc(root_size);
        if (fseeko(f, base + (uint64_t)root * 2048, SEEK_SET) == 0 &&
            fread(dir, 1, root_size, f) == root_size) {
            /* Entries are 4-byte aligned; look at each for default.xbe */
            for (uint32_t o = 0; o + 14 + 11 <= root_size && !found; o += 4) {
                if (dir[o + 13] == 11 &&
                    !g_ascii_strncasecmp((char *)dir + o + 14, "default.xbe",
                                         11) &&
                    !(dir[o + 12] & 0x10)) {
                    uint8_t head[4096];
                    uint32_t sector = ldl_le_p(dir + o + 4);
                    if (fseeko(f, base + (uint64_t)sector * 2048,
                               SEEK_SET) == 0 &&
                        fread(head, 1, sizeof(head), f) == sizeof(head)) {
                        found = xbe_certificate(head, sizeof(head), xbe);
                    }
                }
            }
        }
        g_free(dir);
    }
    fclose(f);
    return found;
}

static bool hdd_has_dashboard;

void xemu_xbonline_probe_hdd(const char *hdd_path)
{
    hdd_has_dashboard = xemu_xbonline_hdd_has_dashboard(hdd_path) == 1;
}

const char *xemu_xbonline_setup_disc(void)
{
    if (hdd_has_dashboard &&
        qemu_access(XEMU_XBONLINE_LOADER_DISC, F_OK) == 0) {
        return XEMU_XBONLINE_LOADER_DISC;
    }
    if (qemu_access(XEMU_XBONLINE_SETUP_DISC, F_OK) == 0) {
        return XEMU_XBONLINE_SETUP_DISC;
    }
    return NULL;
}

bool xemu_xbonline_is_system_disc(const char *path)
{
    return g_str_has_suffix(path, XEMU_XBONLINE_SETUP_DISC) ||
           g_str_has_suffix(path, XEMU_XBONLINE_LOADER_DISC) ||
           g_str_has_suffix(path, XEMU_XBONLINE_UPDATE_DISC);
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

