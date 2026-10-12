/*
 * xemu XB Online helpers
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef XEMU_XBONLINE_H
#define XEMU_XBONLINE_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Check whether the hard disk image holds an Xbox Live machine account.
 * Returns 1 if it does, 0 if it does not, -1 if the image could not be read.
 */
int xemu_xbonline_hdd_has_account(const char *hdd_path);

/*
 * Choose the disc image to boot from the games directory when none is
 * configured. Returns a newly allocated path, or NULL if there is no obvious
 * choice.
 */
char *xemu_xbonline_pick_game(const char *games_dir);

#ifdef __cplusplus
}
#endif

#endif
