/*
 * xemu NV2A shader precache
 *
 * Builds the shaders a game needs before it asks for them, so new effects
 * do not stall the game while their shader compiles. The shaders to build
 * come from a list shipped next to the emulator (shader-list.bin). The list
 * holds only the emulated GPU state of each shader, so it works on any
 * graphics card; the compiled result is kept in the normal shader cache.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef HW_XBOX_NV2A_SHADER_PRECACHE_H
#define HW_XBOX_NV2A_SHADER_PRECACHE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Shipped list of shaders, in the emulator's base folder */
#define NV2A_SHADER_LIST_FILE "shader-list.bin"

/* The shipped list was made from Black Arrow; Rainbow Six 3 shares its engine */
#define NV2A_SHADER_TITLE_BLACK_ARROW 0x55530037u
#define NV2A_SHADER_TITLE_RAINBOW_SIX_3 0x55530013u

/*
 * This PC's own lists, same format: every shader a game needed here, one
 * file per game (shader-list-mine-<title id>.bin). Updates never replace
 * them, and Clear Cache keeps them unless asked.
 *
 * The one list of builds 21 to 24: since build 25 a copy of Black Arrow's,
 * so a rollback to those builds keeps its shaders.
 */
#define NV2A_SHADER_LEARNED_FILE "shader-list-mine-v2.bin"

/*
 * The own list of builds up to 20 (xemu 0.8.137 layout). Never written, so a
 * rollback to those builds still finds it; what they add to it is merged
 * into NV2A_SHADER_LEARNED_FILE at every start.
 */
#define NV2A_SHADER_LEARNED_V1_FILE "shader-list-mine.bin"

/*
 * Crash guard: the shader being precached is written here, and the file is
 * removed when the run ends. Found at the next start, that shader crashed
 * the driver, so it goes in the skip file and is never precached again.
 */
#define NV2A_SHADER_GUARD_FILE "shader-precache-running.txt"
#define NV2A_SHADER_SKIP_FILE "shader-skip.txt"

typedef struct ShaderPrecacheStatus {
    bool supported;      /* the OpenGL renderer is running */
    bool running;        /* counting or building in the background */
    bool building;       /* building, not only counting */
    bool scanned;        /* the counts below are filled in */
    int list_total;      /* list shaders (every scale), -1 if no list */
    int list_missing;    /* of those, not built on this PC yet */
    int done;            /* built so far in this run */
    int failed;          /* could not be built in this run */
    int disk_count;      /* shaders in the cache on disk */
    uint64_t disk_bytes; /* their size */
    uint32_t title;      /* the game the last run was for, 0 = none */
    int learned_count;   /* shaders in this PC's list for the loaded game */
    int skipped_count;   /* shaders skipped since they crashed a precache */
    char gpu[128];       /* graphics card and maker */
    char driver[128];    /* driver version */
    char error[160];     /* why the last run stopped early, or empty */
} ShaderPrecacheStatus;

/* Count what is in the list and the cache. Runs in the background. */
void nv2a_shader_precache_scan(void);

/* Build the shaders of the list that are not in the cache yet */
bool nv2a_shader_precache_start(void);

/* Stop building after the current shader */
void nv2a_shader_precache_stop(void);

/*
 * The game on the loaded disc (title id, 0 for none): shaders built from now
 * on go in its list, and the next precache builds only its shaders.
 */
void nv2a_shader_precache_set_title(uint32_t title);

void nv2a_shader_precache_get_status(ShaderPrecacheStatus *status);

/*
 * Delete the compiled shaders on disk, and with forget_learned also this
 * PC's own list and the skip file. Shaders already loaded stay in use until
 * the emulator restarts. Not possible while building.
 */
bool nv2a_shader_cache_clear(bool forget_learned);

#ifdef __cplusplus
}
#endif

#endif
