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

#include <stdint.h>

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

/* A button the panel shows under an item */
typedef enum XbCheckAction {
    XBCHECK_ACTION_NONE,
    XBCHECK_ACTION_OPEN_GAMES_FOLDER,
    /* Copies XbCheckItem.sha256; only a convenience, nothing to fix */
    XBCHECK_ACTION_COPY_SHA256,
} XbCheckAction;

#define XBCHECK_DETAIL_LEN 320

typedef struct XbCheckItem {
    const char *name;
    XbCheckState state;
    XbCheckAction action;
    char detail[XBCHECK_DETAIL_LEN];
    char sha256[65]; /* for XBCHECK_ACTION_COPY_SHA256 */
} XbCheckItem;

#define XBCHECK_MAX_ITEMS 8

/* Start a check in the background, unless one is already running */
void xemu_xbcheck_start(void);

/*
 * Copy the current results. Returns the number of items; *running is set
 * while the check has not finished.
 */
int xemu_xbcheck_get(XbCheckItem *items, int max, bool *running);

/* Open the games folder in the file manager, creating it if needed */
void xemu_xbcheck_open_games_folder(void);

/* Called by the network setup when a forwarded port could not be opened */
void xemu_xbcheck_note_port_busy(int port);

/*
 * Ask a DNS server (IPv4 address or name) for the address of a name, waiting
 * up to timeout_ms per try (two tries). Returns 1 with the first address
 * (host order), 0 if the server answered without one, -1 if it did not answer.
 */
int xemu_xbcheck_dns_query(const char *server, const char *name,
                            uint32_t *addr, int timeout_ms);

/*
 * Called by the DNS redirect filter when it answered an Xbox Live name from
 * its built-in list: adds a note to the "XB Online server" result
 */
void xemu_xbcheck_note_dns_fallback(void);

#ifdef __cplusplus
}
#endif

#endif
