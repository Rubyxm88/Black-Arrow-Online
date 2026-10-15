/*
 * xemu XB Online connection check
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef XEMU_XBCHECK_H
#define XEMU_XBCHECK_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum XbCheckState {
    XBCHECK_RUNNING,
    XBCHECK_OK,
    XBCHECK_INFO, /* nothing wrong, but worth knowing */
    XBCHECK_WARN,
    XBCHECK_FAIL,
} XbCheckState;

typedef struct XbCheckItem {
    const char *name;
    XbCheckState state;
    char detail[320];
} XbCheckItem;

#define XBCHECK_MAX_ITEMS 8

/* Start a check in the background, unless one is already running */
void xemu_xbcheck_start(void);

/*
 * Copy the current results. Returns the number of items; *running is set
 * while the check has not finished.
 */
int xemu_xbcheck_get(XbCheckItem *items, int max, bool *running);

/* Called by the network setup when a forwarded port could not be opened */
void xemu_xbcheck_note_port_busy(int port);

#ifdef __cplusplus
}
#endif

#endif
