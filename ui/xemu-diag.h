/*
 * xemu diagnostics log
 *
 * A text file a player records while playing (RSO Settings > Diagnostics)
 * and sends in when the game runs badly. It starts with the PC, the package
 * and the settings, then has one line per second (frame rate, the work that
 * can cause a hitch, CPU and memory use), a line for every slow frame with
 * its likely cause, events (markers, focus changes, disc changes, connection
 * check results, shader precache) and a summary when recording stops.
 *
 * Freeze reports: the same lines are also kept in memory for the last
 * minute, without writing anything. When the game shows no new picture for
 * FREEZE_SECONDS while the emulator runs, a short report with those lines
 * and what the emulated CPU and GPU were doing is written to the logs folder
 * (RSO Settings > Diagnostics > Save a report when the game freezes).
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef XEMU_DIAG_H
#define XEMU_DIAG_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <glib.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Why a slow frame was slow, judged from the work done during it */
typedef enum XemuDiagCause {
    XEMU_DIAG_CAUSE_SHADER,    /* building a new shader */
    XEMU_DIAG_CAUSE_COPYBACK,  /* copying the screen back from the GPU */
    XEMU_DIAG_CAUSE_CODE,      /* translating game code */
    XEMU_DIAG_CAUSE_LOADING,   /* loading screen (almost nothing drawn) */
    XEMU_DIAG_CAUSE_PAUSE,     /* no frame for seconds: paused, menu, drag */
    XEMU_DIAG_CAUSE_QUERY,     /* waiting for occlusion query results */
    XEMU_DIAG_CAUSE_AUDIO,     /* the game waiting for the audio thread */
    XEMU_DIAG_CAUSE_GAME_CPU,  /* the emulated CPU busy the whole frame */
    XEMU_DIAG_CAUSE_OTHER,
    XEMU_DIAG_CAUSE__COUNT
} XemuDiagCause;

/* Start recording to a new file in the logs folder. False on failure. */
bool xemu_diag_start(void);

/* Start recording to the given file (XEMU_PERF_LOG) */
bool xemu_diag_start_path(const char *path);

/* Write the summary and close the file */
void xemu_diag_stop(void);

bool xemu_diag_active(void);

/* Seconds since recording started */
double xemu_diag_seconds(void);

/* Path of the file being written, or of the last one written */
const char *xemu_diag_path(void);

/* Open the logs folder in the file manager */
void xemu_diag_open_folder(void);

/* The player marked a moment (F9) */
void xemu_diag_mark(void);

/* An "EVENT" line */
void xemu_diag_event(const char *fmt, ...) G_GNUC_PRINTF(1, 2);

/* Write the connection check results, when a check has finished */
void xemu_diag_write_check(void);

/*
 * Called from the UI each frame: starts the background capture, watches
 * focus and disc changes
 */
void xemu_diag_poll(void);

/* Whether the frame logger should report frames (a log or freeze reports) */
bool xemu_diag_recording(void);

/*
 * From the frame logger, once per frame: the gap since the previous frame,
 * the work done in it (per XemuPerfKind) and the GPU counters of the frame
 * (gpu_n of them, named by nv2a_profile_get_counter_name)
 */
void xemu_diag_frame(int64_t now_us, int64_t gap_us, bool slow,
                     XemuDiagCause cause, const int64_t *count,
                     const int64_t *ns, const int64_t *max_ns, const int *gpu,
                     int gpu_n);

const char *xemu_diag_cause_name(XemuDiagCause cause);

/* Seconds since this run wrote a freeze report, or -1 if it has not */
double xemu_diag_freeze_age(void);

/* The newest freeze report in the logs folder, or NULL. Free with g_free. */
char *xemu_diag_last_freeze_report(void);

/* Open the newest freeze report. False if there is none. */
bool xemu_diag_open_freeze_report(void);

#ifdef __cplusplus
}
#endif

#endif
