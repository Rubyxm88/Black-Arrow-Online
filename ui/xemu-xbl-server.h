/*
 * XB Online (xb.live) server address
 *
 * The console's DNS lookups go to XB Online's server, which also answers the
 * Xbox Live names with its own address. Its address can change (it moved on
 * 2026-11-17), so xemu finds it at start instead of only trusting a built-in
 * number, in this order, using the first that answers like XB Online:
 *
 *   1. the name online.xb.live, looked up through this PC's normal DNS
 *   2. the address file on GitHub (name= and/or address=), so the address
 *      can change without a new build
 *   3. the last address that worked, saved in xbl-server.txt
 *   4. the address built into this build
 *
 * When none answers (offline, or a network that answers DNS itself) the first
 * address found in that order is used without saving it.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef XEMU_XBL_SERVER_H
#define XEMU_XBL_SERVER_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The address to use now (IPv4 text, never NULL). Before the first look ends
 * this is the saved last good address, else the built-in one.
 */
const char *xemu_xbl_server(void);

/* Where the address came from, for logs and the connection check */
const char *xemu_xbl_server_source(void);

/* Starts the first look in the background (once; later calls do nothing) */
void xemu_xbl_server_start(void);

/* Waits up to ms for the first look to end. True if it ended. */
bool xemu_xbl_server_wait(int ms);

/*
 * Looks again in the background, at most once every two minutes: the server
 * stopped answering. A new working address is handed to the DNS redirect.
 */
void xemu_xbl_server_recheck(void);

#ifdef __cplusplus
}
#endif

#endif
