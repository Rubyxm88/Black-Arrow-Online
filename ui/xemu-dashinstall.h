/*
 * xemu: one-time XBL Dashboard install
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef XEMU_DASHINSTALL_H
#define XEMU_DASHINSTALL_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Whether xemu should install the XBL Dashboard at this start: the package
 * has the installer disc, the hard disk still boots another dashboard, and
 * the install has not already failed twice. Call after
 * xemu_xbonline_probe_hdd().
 */
bool xemu_dashinstall_wanted(void);

/*
 * Run the install: the machine is booting the installer disc (the caller
 * put it in the drive). xemu covers the screen with its own progress panel,
 * presses A in the installer, waits for it to finish, then takes the disc
 * out and restarts into the XBL Dashboard.
 */
void xemu_dashinstall_begin(void);

/* Put the installer disc in now, restart, and run the install */
void xemu_dashinstall_start_now(void);

/* Whether the install is running */
bool xemu_dashinstall_active(void);

/* Bytes written to the hard disk since xemu started */
uint64_t xemu_dashinstall_hdd_written(void);

/* Write the hard disk image's tables out */
void xemu_dashinstall_hdd_flush(void);

/* Console restarts so far (counted from the first call) */
int xemu_dashinstall_resets(void);

#ifdef __cplusplus
}
#endif

#endif
