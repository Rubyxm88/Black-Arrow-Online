/*
 * xemu diagnostics log
 *
 * A text file a player records while playing (XB Online > Diagnostics Log)
 * and sends in when the game runs badly. It starts with the PC, the package
 * and the settings, then has one line per second (frame rate, the work that
 * can cause a hitch, CPU and memory use), a line for every slow frame with
 * its likely cause, events (markers, focus changes, disc changes, connection
 * check results, shader precache) and a summary when recording stops.
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

/* Called from the UI each frame: watches focus and disc changes */
void xemu_diag_poll(void);

/*
 * For the frame logger: the open file, locked, or NULL when not recording.
 * *generation changes every time a new recording starts and *start_us is
 * when it started (QEMU_CLOCK_REALTIME). Unlock after writing.
 */
FILE *xemu_diag_lock(int *generation, int64_t *start_us);
void xemu_diag_unlock(void);

/* With the lock held: count a frame for the summary */
void xemu_diag_count_frame(int64_t now_us, int64_t gap_us, bool slow,
                           XemuDiagCause cause);

/* With the lock held: count a second for the summary */
void xemu_diag_count_second(int frames);

/* With the lock held: " cpu=..% sys_cpu=..% mem_mb=.." for the second line */
void xemu_diag_print_system(FILE *log);

const char *xemu_diag_cause_name(XemuDiagCause cause);

#ifdef __cplusplus
}
#endif

#endif
