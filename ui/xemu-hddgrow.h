/*
 * xemu: one-time hard disk grow, 8 GiB -> 64 GiB with an empty F: drive
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef XEMU_HDDGROW_H
#define XEMU_HDDGROW_H

/* Grows a stock 8 GiB qcow2 hard disk to 64 GiB and formats F:. Call once
 * the block layer and main loop are up, before the hard disk is attached.
 * Does nothing on any other disk, or when F: already exists. */
void xemu_hdd_grow(const char *path);

#endif
