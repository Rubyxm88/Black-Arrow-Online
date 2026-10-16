/*
 * xemu game cache
 *
 * The Xbox keeps a copy of files a game reads from its disc on one of the
 * cache partitions X, Y and Z of the hard disk. Which game owns which
 * partition is in a table at sector 4 of the disk. The copy is kept until
 * the partition is needed for another game, so after switching to a changed
 * disc image of the same game (a modded ISO) the game keeps reading the old
 * files.
 *
 * Each time a disc is loaded, its image is compared with the one that game
 * last ran from. When it differs, the game's entry in the table is removed,
 * and the Xbox formats a partition and copies the files again from the new
 * disc when the game starts.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef XEMU_GAMECACHE_H
#define XEMU_GAMECACHE_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Check the disc again once the machine is created (at startup) */
void xemu_gamecache_init(void);

/*
 * Compare the disc image with the one its game last ran from and empty the
 * game's cache if it changed. Needs the main loop lock.
 */
void xemu_gamecache_check_disc(const char *iso_path);

/*
 * Empty the cache of a game now. Needs the main loop lock. Returns 1 if the
 * game had a cache, 0 if it had none, -1 if the hard disk could not be read
 * or written.
 */
int xemu_gamecache_empty(uint32_t title_id);

/*
 * Whether the game has a cache on the hard disk: 1 yes, 0 no, -1 the hard
 * disk could not be read. Needs the main loop lock.
 */
int xemu_gamecache_has(uint32_t title_id);

/*
 * Title ID and name of the game in the disc drive. False if there is no
 * disc or it is not a game.
 */
bool xemu_gamecache_disc_title(uint32_t *title_id, char *name, size_t len);

#ifdef __cplusplus
}
#endif

#endif
