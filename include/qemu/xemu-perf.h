/*
 * xemu performance log counters
 *
 * Time spent on work that can cause a hitch: compiling a new shader, loading
 * a cached one, mixing audio and translating guest code the first time it
 * runs. The NV2A profiler (hw/xbox/nv2a/pgraph/profile.c) reads and clears
 * them each frame while a diagnostics log records or freeze reports are on
 * (ui/xemu-diag.h).
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef QEMU_XEMU_PERF_H
#define QEMU_XEMU_PERF_H

#include "qemu/atomic.h"
#include "qemu/timer.h"

enum XemuPerfKind {
    XEMU_PERF_SHADER_GEN,   /* new shader compiled */
    XEMU_PERF_SHADER_LOAD,  /* shader loaded from the cache */
    XEMU_PERF_AUDIO_VP,     /* audio voices mixed */
    XEMU_PERF_AUDIO_DSP,    /* audio effects DSP */
    XEMU_PERF_TCG,          /* guest code translated */
    XEMU_PERF_SHADER_ASYNC, /* new shader started or finished in background */
    XEMU_PERF_DRAW_SKIPPED, /* draw skipped while its shader builds */
    XEMU_PERF_COPYBACK,     /* screen copied back from the GPU to Xbox RAM */
    XEMU_PERF_SURF_UPLOAD,  /* screen copied from Xbox RAM to the GPU */
    XEMU_PERF_QUERY_WAIT,   /* waiting for occlusion query results */
    XEMU_PERF_AUDIO_LOCK,   /* game waiting for the audio thread's lock */
    XEMU_PERF_AUDIO_DROPOUT, /* sound output ran dry (count only) */
    XEMU_PERF_BQL_WAIT,     /* game CPU waiting for the emulator's main lock */
    XEMU_PERF_CODE_WRITE,   /* game wrote to a page holding translated code */
    XEMU_PERF_CODE_DROPPED, /* translated code thrown away (count only) */
    XEMU_PERF_CODE_DMA,     /* a device wrote over translated code (count) */
    XEMU_PERF_CODE_FLUSH,   /* all translated code thrown away (count only) */
    XEMU_PERF_IO_RECOMPILE, /* code retranslated around a device access */
    XEMU_PERF__COUNT
};

extern bool xemu_perf_enabled;
extern int64_t xemu_perf_ns[XEMU_PERF__COUNT];
extern int64_t xemu_perf_count[XEMU_PERF__COUNT];
extern int64_t xemu_perf_max_ns[XEMU_PERF__COUNT];

static inline int64_t xemu_perf_start(void)
{
    return qatomic_read(&xemu_perf_enabled) ? get_clock() : 0;
}

static inline void xemu_perf_end(enum XemuPerfKind kind, int64_t start)
{
    if (!start) {
        return;
    }
    int64_t ns = get_clock() - start;
    qatomic_add(&xemu_perf_ns[kind], ns);
    qatomic_inc(&xemu_perf_count[kind]);
    if (ns > qatomic_read(&xemu_perf_max_ns[kind])) {
        qatomic_set(&xemu_perf_max_ns[kind], ns);
    }
}

/*
 * While recording: a block of game code translated at guest address pc from
 * RAM page ram_page, and the game writing to a RAM page that held translated
 * code. The diagnostics log summary lists the most frequent ones.
 */
void xemu_perf_code_translated(uint64_t pc, uint64_t ram_page);
void xemu_perf_code_written(uint64_t ram_page);

/* Count something that has no duration */
static inline void xemu_perf_event(enum XemuPerfKind kind)
{
    if (qatomic_read(&xemu_perf_enabled)) {
        qatomic_inc(&xemu_perf_count[kind]);
    }
}

#endif
