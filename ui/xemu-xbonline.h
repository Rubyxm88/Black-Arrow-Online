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
 * Disc image of the XBL Dashboard, kept next to the executable. It is used
 * for Xbox Live account setup and is never treated as a game.
 */
#define XEMU_XBONLINE_SETUP_DISC "XBL Dashboard.iso"

/*
 * Small disc image that starts the XBL Dashboard installed on the hard disk.
 * It is preferred when the hard disk has the dashboard, because only that
 * copy can update itself.
 */
#define XEMU_XBONLINE_LOADER_DISC "XBL Dashboard Loader.iso"

/*
 * Optional disc image, also kept next to the executable, that installs the
 * title update and downloadable content of the main game.
 */
#define XEMU_XBONLINE_UPDATE_DISC "Black Arrow Update and Maps.iso"

/*
 * Optional disc image next to the executable that installs the title update
 * and downloadable maps of Rainbow Six 3 ("Old Bow" in the menu).
 */
#define XEMU_XBONLINE_RS3_UPDATE_DISC "Rainbow Six 3 Update and Maps.iso"

/* XB Online web pages */
#define XEMU_XBONLINE_URL_DASHBOARD "https://xb.live/dashboard"
#define XEMU_XBONLINE_URL_CREATE "https://xb.live/xb-online"
#define XEMU_XBONLINE_URL_LINK "https://xb.live/link"

/*
 * Check whether the hard disk image holds an Xbox Live machine account.
 * Returns 1 if it does, 0 if it does not, -1 if the image could not be read.
 */
int xemu_xbonline_hdd_has_account(const char *hdd_path);

/*
 * Gamertags of the Xbox Live accounts on the hard disk image, joined with
 * ", " into out. Returns how many, or -1 if the image could not be read.
 */
int xemu_xbonline_hdd_account_names(const char *hdd_path, char *out,
                                    size_t out_len);

/*
 * Check whether the XBL Dashboard is installed on the hard disk image.
 * Returns 1 if it is, 0 if it is not, -1 if the image could not be read.
 */
int xemu_xbonline_hdd_has_dashboard(const char *hdd_path);

/* Title ID of Rainbow Six 3: Black Arrow */
#define XEMU_XBONLINE_TITLE_ID 0x55530037

/* Title update version 2.01, installed by the update disc */
#define XEMU_XBONLINE_TITLE_UPDATE_VERSION 0x201

/* Title ID of Rainbow Six 3 */
#define XEMU_XBONLINE_RS3_TITLE_ID 0x55530013

/*
 * Rainbow Six 3 title updates on its update disc: "RF" for disc version 1
 * (all regions), "Ger" for disc version 2 (PAL German). Discs of version 3
 * and later already hold the update and must not get one.
 */
#define XEMU_XBONLINE_RS3_UPDATE_RF 0x501
#define XEMU_XBONLINE_RS3_UPDATE_GER 0x502

/* The first Scharins map pack, which breaks multiplayer with Scharins v2 */
#define XEMU_XBONLINE_RS3_OLD_SCHARINS "5553001300000006"

typedef struct XbOnlineXbe {
    uint32_t title_id;
    uint32_t version;
    char name[40];
} XbOnlineXbe;

/*
 * Look for the title update of a game on the hard disk image and count its
 * downloadable content. Returns 1 if the update is there (its certificate in
 * *xbe), 0 if it is not, -1 if the image could not be read.
 */
int xemu_xbonline_hdd_title_update(const char *hdd_path, uint32_t title_id,
                                   XbOnlineXbe *xbe, int *content_count);

/*
 * Whether a downloadable content package (its offering ID as 16 hex digits)
 * of a game is on the hard disk image. Returns 1 if it is, 0 if it is not,
 * -1 if the image could not be read.
 */
int xemu_xbonline_hdd_has_content(const char *hdd_path, uint32_t title_id,
                                  const char *offering);

/* Read the certificate of default.xbe on an Xbox disc image */
bool xemu_xbonline_disc_xbe(const char *iso_path, XbOnlineXbe *xbe);

/*
 * Look at the hard disk image once, before the machine starts using it, so
 * xemu_xbonline_setup_disc() knows which disc to offer.
 */
void xemu_xbonline_probe_hdd(const char *hdd_path);

/*
 * Disc image to boot for Xbox Live account setup, or NULL if none is
 * available.
 */
const char *xemu_xbonline_setup_disc(void);

/* Whether the path names one of the dashboard or update discs, not a game */
bool xemu_xbonline_is_system_disc(const char *path);

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
