/*
 * xemu diagnostics log: CPU use per emulator thread and of other programs
 *
 * Which part of the emulator is busy: the emulated Xbox CPU, the graphics
 * thread, the window, the audio thread and its voice workers, as a share of
 * one PC core. Threads are told apart by the names QEMU gives them. Other
 * programs on the PC are sampled less often, as a share of the whole PC.
 * Windows only; elsewhere these print nothing.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef XEMU_DIAG_THREADS_H
#define XEMU_DIAG_THREADS_H

#include <glib.h>

/* Forget the totals, for a new log. Main loop. */
void xemu_diag_threads_reset(void);

/* From the UI thread, so its thread is known */
void xemu_diag_threads_mark_ui(void);

/*
 * Once a second from the main loop: appends
 * " thread_cpu=game_cpu:N,gpu:N,... busiest_other=name:N" for the time since
 * the last call (percent of one core)
 */
void xemu_diag_threads_sample(GString *s);

/* Averages and peaks since the reset, as SUMMARY lines */
void xemu_diag_threads_summary(GString *s);

/*
 * How busy the emulated CPU's thread was since the last call, 0 to 1, or
 * -1 if unknown. From the render thread only, once per frame.
 */
double xemu_diag_threads_game_cpu(void);

/*
 * Every few seconds from the main loop: on a thread of its own, look for new
 * emulator threads and at the other programs using the PC, and log an EVENT
 * when those used a fair share since the last scan. Does nothing while the
 * last scan is still running.
 */
void xemu_diag_threads_scan(void);

/* The busiest other programs since the reset, as SUMMARY lines */
void xemu_diag_programs_summary(GString *s);

#endif
