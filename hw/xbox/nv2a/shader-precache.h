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

/*
 * This PC's own list, same format: every shader the game needed here.
 * Updates never replace it, and Clear Cache keeps it unless asked.
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
    int learned_count;   /* shaders in this PC's own list */
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
