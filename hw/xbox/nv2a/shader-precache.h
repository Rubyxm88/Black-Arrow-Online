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

typedef struct ShaderPrecacheStatus {
    bool supported;      /* the OpenGL renderer is running */
    bool running;        /* counting or building in the background */
    bool building;       /* building, not only counting */
    bool scanned;        /* the counts below are filled in */
    int list_total;      /* shaders in the list, -1 if there is no list */
    int list_missing;    /* of those, not built on this PC yet */
    int done;            /* built so far in this run */
    int failed;          /* could not be built in this run */
    int disk_count;      /* shaders in the cache on disk */
    uint64_t disk_bytes; /* their size */
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
 * Delete the shader cache on disk. Shaders already loaded stay in use until
 * the emulator restarts. Not possible while building.
 */
bool nv2a_shader_cache_clear(void);

#ifdef __cplusplus
}
#endif

#endif
