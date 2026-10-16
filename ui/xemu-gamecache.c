/*
 * xemu game cache
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/notify.h"
#include "system/block-backend.h"
#include "system/system.h"
#include <glib/gstdio.h>

#include "xemu-gamecache.h"
#include "xemu-xbonline.h"
#include "xemu-settings.h"
#include "xemu-diag.h"
#include "xemu-notifications.h"

#define HDD_NAME "ide0-hd0"

/* The table of cache partitions, at sector 4 of the hard disk */
#define TABLE_OFFSET 0x800
#define TABLE_SIZE 512
#define TABLE_SIGNATURE 0x97315286
#define TABLE_FIRST_ENTRY 8
#define ENTRY_SIZE 8

/* What each game last ran from, next to the executable */
#define RECORD_FILE "game-cache.txt"

static BlockBackend *hdd(void)
{
    BlockBackend *blk = blk_by_name(HDD_NAME);
    return blk && blk_is_available(blk) ? blk : NULL;
}

/* Offset of the game's entry in the table, or -1 */
static int find_entry(const uint8_t *table, uint32_t title_id)
{
    for (int o = TABLE_FIRST_ENTRY; o + ENTRY_SIZE <= TABLE_SIZE;
         o += ENTRY_SIZE) {
        if (ldl_le_p(table + o) == title_id) {
            return o;
        }
    }
    return -1;
}

int xemu_gamecache_has(uint32_t title_id)
{
    uint8_t table[TABLE_SIZE];
    BlockBackend *blk = hdd();

    if (!blk || blk_pread(blk, TABLE_OFFSET, TABLE_SIZE, table, 0) < 0) {
        return -1;
    }
    /* A disk that has never cached anything has no table yet */
    if (ldl_le_p(table) != TABLE_SIGNATURE) {
        return 0;
    }
    return find_entry(table, title_id) >= 0;
}

int xemu_gamecache_empty(uint32_t title_id)
{
    uint8_t table[TABLE_SIZE];
    BlockBackend *blk = hdd();
    int o;

    if (!blk || blk_pread(blk, TABLE_OFFSET, TABLE_SIZE, table, 0) < 0) {
        return -1;
    }
    if (ldl_le_p(table) != TABLE_SIGNATURE) {
        return 0;
    }
    /*
     * Forgetting which partition the game owns is enough: the Xbox then
     * gives the game a partition, formats it and copies the files again.
     * A game listed twice would be odd, but forget every entry anyway.
     */
    o = find_entry(table, title_id);
    if (o < 0) {
        return 0;
    }
    while (o >= 0) {
        stl_le_p(table + o, 0);
        o = find_entry(table, title_id);
    }
    if (blk_pwrite(blk, TABLE_OFFSET, TABLE_SIZE, table, 0) < 0 ||
        blk_flush(blk) < 0) {
        return -1;
    }
    return 1;
}

/*
 * Fingerprint of a disc image: its size and modification time, and the
 * volume descriptor and root folder of the Xbox file system, which change
 * whenever the image is built again. Reads a few kilobytes.
 */
static char *disc_fingerprint(const char *iso_path)
{
    static const uint64_t bases[] = { 0, 0x18300000ULL, 0x1040000ULL,
                                      0xFD90000ULL };
    GChecksum *sum;
    GStatBuf st;
    FILE *f;
    char *result = NULL;
    uint8_t vd[2048];

    if (g_stat(iso_path, &st) != 0) {
        return NULL;
    }
    f = qemu_fopen(iso_path, "rb");
    if (!f) {
        return NULL;
    }
    sum = g_checksum_new(G_CHECKSUM_SHA1);
    {
        int64_t size = st.st_size, mtime = st.st_mtime;
        g_checksum_update(sum, (const guchar *)&size, sizeof(size));
        g_checksum_update(sum, (const guchar *)&mtime, sizeof(mtime));
    }
    for (int b = 0; b < ARRAY_SIZE(bases); b++) {
        uint64_t base = bases[b];
        uint32_t root, root_size;
        uint8_t *dir;

        if (fseeko(f, base + 32 * 2048, SEEK_SET) ||
            fread(vd, 1, sizeof(vd), f) != sizeof(vd) ||
            memcmp(vd, "MICROSOFT*XBOX*MEDIA", 20)) {
            continue;
        }
        g_checksum_update(sum, vd, sizeof(vd));
        root = ldl_le_p(vd + 20);
        root_size = ldl_le_p(vd + 24);
        if (root_size > 0 && root_size <= 1024 * 1024) {
            dir = g_malloc(root_size);
            if (fseeko(f, base + (uint64_t)root * 2048, SEEK_SET) == 0 &&
                fread(dir, 1, root_size, f) == root_size) {
                g_checksum_update(sum, dir, root_size);
            }
            g_free(dir);
        }
        result = g_strdup(g_checksum_get_string(sum));
        break;
    }
    g_checksum_free(sum);
    fclose(f);
    return result;
}

static char *record_path(void)
{
    return g_build_filename(xemu_settings_get_base_path(), RECORD_FILE, NULL);
}

/* The fingerprint the game last ran from, or NULL */
static char *record_get(uint32_t title_id)
{
    char *path = record_path(), *text = NULL, *found = NULL;

    if (g_file_get_contents(path, &text, NULL, NULL)) {
        char **lines = g_strsplit(text, "\n", -1);
        for (int i = 0; lines[i] && !found; i++) {
            unsigned int id;
            char fp[41];
            if (sscanf(lines[i], "%8x %40s", &id, fp) == 2 && id == title_id) {
                found = g_strdup(fp);
            }
        }
        g_strfreev(lines);
        g_free(text);
    }
    g_free(path);
    return found;
}

static void record_set(uint32_t title_id, const char *fingerprint,
                       const char *iso_path)
{
    char *path = record_path(), *text = NULL;
    GString *out = g_string_new(
        "# The disc image each game last ran from (see Troubleshoot > "
        "Caches > Empty Game Cache)\n");

    if (g_file_get_contents(path, &text, NULL, NULL)) {
        char **lines = g_strsplit(text, "\n", -1);
        for (int i = 0; lines[i]; i++) {
            unsigned int id;
            if (sscanf(lines[i], "%8x", &id) == 1 && id != title_id) {
                g_string_append_printf(out, "%s\n", lines[i]);
            }
        }
        g_strfreev(lines);
        g_free(text);
    }
    {
        char *name = g_path_get_basename(iso_path);
        g_string_append_printf(out, "%08x %s %s\n", title_id, fingerprint,
                               name);
        g_free(name);
    }
    if (!g_file_set_contents(path, out->str, out->len, NULL)) {
        fprintf(stderr, "game cache: could not write %s\n", path);
    }
    g_string_free(out, true);
    g_free(path);
}

void xemu_gamecache_check_disc(const char *iso_path)
{
    XbOnlineXbe xbe;
    char *fingerprint, *last;
    int has;

    if (!iso_path || !iso_path[0] || xemu_xbonline_is_system_disc(iso_path) ||
        !xemu_xbonline_disc_xbe(iso_path, &xbe)) {
        return;
    }
    fingerprint = disc_fingerprint(iso_path);
    if (!fingerprint) {
        return;
    }
    last = record_get(xbe.title_id);
    if (last && !strcmp(last, fingerprint)) {
        g_free(last);
        g_free(fingerprint);
        return;
    }

    /*
     * A changed disc, or the first time this game is seen here: a cache
     * left from an older disc image may be on the hard disk, so empty it
     * once as well.
     */
    has = xemu_gamecache_has(xbe.title_id);
    if (has < 0) {
        /* No hard disk yet: try again on the next load */
        g_free(last);
        g_free(fingerprint);
        return;
    }
    if (has == 1) {
        int r = xemu_gamecache_empty(xbe.title_id);
        fprintf(stderr, "game cache: %s for %s (%08x), %s\n",
                r == 1 ? "emptied" : "could not empty", xbe.name,
                xbe.title_id, last ? "the disc changed" : "first run");
        xemu_diag_event("Game cache %s for %s (%s)",
                        r == 1 ? "emptied" : "could not be emptied", xbe.name,
                        last ? "the disc changed" : "first run");
        if (r != 1) {
            g_free(last);
            g_free(fingerprint);
            return;
        }
        if (last) {
            char *msg = g_strdup_printf(
                "New %s disc: the game copies its files again, the first "
                "start takes a little longer", xbe.name);
            xemu_queue_notification(msg);
            g_free(msg);
        }
    }
    record_set(xbe.title_id, fingerprint, iso_path);
    g_free(last);
    g_free(fingerprint);
}

bool xemu_gamecache_disc_title(uint32_t *title_id, char *name, size_t len)
{
    const char *path = g_config.sys.files.dvd_path;
    XbOnlineXbe xbe;

    if (!path || !path[0] || xemu_xbonline_is_system_disc(path) ||
        !xemu_xbonline_disc_xbe(path, &xbe)) {
        return false;
    }
    *title_id = xbe.title_id;
    if (name && len) {
        g_strlcpy(name, xbe.name, len);
    }
    return true;
}

static void machine_ready(Notifier *notifier, void *data)
{
    xemu_gamecache_check_disc(g_config.sys.files.dvd_path);
}

static Notifier machine_ready_notifier = { .notify = machine_ready };

void xemu_gamecache_init(void)
{
    qemu_add_machine_init_done_notifier(&machine_ready_notifier);
}
