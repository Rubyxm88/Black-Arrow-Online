/*
 * QEMU Geforce NV2A profiling helpers
 *
 * Copyright (c) 2020-2024 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "hw/xbox/nv2a/nv2a_int.h"
#include "qemu/xemu-perf.h"
#include "ui/xemu-settings.h"
#include "ui/xemu-diag.h"

NV2AStats g_nv2a_stats;

bool xemu_perf_enabled;
int64_t xemu_perf_ns[XEMU_PERF__COUNT];
int64_t xemu_perf_count[XEMU_PERF__COUNT];
int64_t xemu_perf_max_ns[XEMU_PERF__COUNT];

/* A frame that takes this long is a hitch and gets a line of its own */
#define PERF_SLOW_FRAME_US 50000

/* The times of the last frames, for the performance overlay */
#define FRAME_TIMES 1024
static int64_t frame_times[FRAME_TIMES];
static unsigned int frame_times_next;

/* The likely cause of a slow frame, from the work done during it */
static XemuDiagCause perf_cause(int64_t gap_us, const int64_t *ns)
{
    const int *counters = g_nv2a_stats.frame_working.counters;

    if (gap_us >= 2000000) {
        return XEMU_DIAG_CAUSE_PAUSE;
    }
    if ((ns[XEMU_PERF_SHADER_GEN] + ns[XEMU_PERF_SHADER_ASYNC]) * 3 >=
        gap_us * 1000) {
        return XEMU_DIAG_CAUSE_SHADER;
    }
    if (ns[XEMU_PERF_TCG] * 3 >= gap_us * 1000) {
        return XEMU_DIAG_CAUSE_CODE;
    }
    if (counters[NV2A_PROF_BEGIN_ENDS] <= 1) {
        return XEMU_DIAG_CAUSE_LOADING;
    }
    /*
     * Most frames copy the screen back a few times, so only blame it when
     * the copies took a good part of the frame
     */
    if ((ns[XEMU_PERF_COPYBACK] + ns[XEMU_PERF_SURF_UPLOAD]) * 3 >=
        gap_us * 1000) {
        return XEMU_DIAG_CAUSE_COPYBACK;
    }
    if (ns[XEMU_PERF_QUERY_WAIT] * 3 >= gap_us * 1000) {
        return XEMU_DIAG_CAUSE_QUERY;
    }
    if (ns[XEMU_PERF_AUDIO_LOCK] * 3 >= gap_us * 1000) {
        return XEMU_DIAG_CAUSE_AUDIO;
    }
    return XEMU_DIAG_CAUSE_OTHER;
}

/*
 * Reports each frame to the diagnostics log (ui/xemu-diag.h), which keeps
 * the per-second lines for a log being recorded (RSO Settings > Record
 * Diagnostics Log, or XEMU_PERF_LOG) and for freeze reports: the gap since the last
 * frame, the work done in it and its GPU counters. A frame that took longer
 * than PERF_SLOW_FRAME_US is slow and gets a likely cause.
 */
static void perf_log_frame(int64_t now)
{
    static bool was_recording;
    static int64_t last_frame;

    if (!xemu_diag_recording()) {
        was_recording = false;
        return;
    }

    /* Work done since the last frame */
    int64_t count[XEMU_PERF__COUNT], ns[XEMU_PERF__COUNT],
            max_ns[XEMU_PERF__COUNT];
    for (int i = 0; i < XEMU_PERF__COUNT; i++) {
        count[i] = qatomic_xchg(&xemu_perf_count[i], 0);
        ns[i] = qatomic_xchg(&xemu_perf_ns[i], 0);
        max_ns[i] = qatomic_xchg(&xemu_perf_max_ns[i], 0);
    }
    if (!was_recording) {
        /* Count from here */
        was_recording = true;
        last_frame = now;
        return;
    }

    int64_t gap = now - last_frame;
    bool slow = gap >= PERF_SLOW_FRAME_US;
    last_frame = now;
    XemuDiagCause cause = slow ? perf_cause(gap, ns) : XEMU_DIAG_CAUSE_OTHER;
    xemu_diag_frame(now, gap, slow, cause, count, ns, max_ns,
                    g_nv2a_stats.frame_working.counters, NV2A_PROF__COUNT);
}

void nv2a_profile_increment(void)
{
    int64_t now = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
    const int64_t fps_update_interval = 250000;
    g_nv2a_stats.last_flip_time = now;

    static int64_t frame_count = 0;
    frame_count++;

    static int64_t ts = 0;
    int64_t delta = now - ts;
    if (delta >= fps_update_interval) {
        g_nv2a_stats.increment_fps = frame_count * 1000000 / delta;
        ts = now;
        frame_count = 0;
    }
}

void nv2a_profile_flip_stall(void)
{
    int64_t now = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
    int64_t render_time = (now-g_nv2a_stats.last_flip_time)/1000;

    perf_log_frame(now);
    frame_times[frame_times_next % FRAME_TIMES] = now;
    qatomic_store_release(&frame_times_next, frame_times_next + 1);

    g_nv2a_stats.frame_working.mspf = render_time;
    g_nv2a_stats.frame_history[g_nv2a_stats.frame_ptr] =
        g_nv2a_stats.frame_working;
    g_nv2a_stats.frame_ptr =
        (g_nv2a_stats.frame_ptr + 1) % NV2A_PROF_NUM_FRAMES;
    g_nv2a_stats.frame_count++;
    memset(&g_nv2a_stats.frame_working, 0, sizeof(g_nv2a_stats.frame_working));
}

int nv2a_profile_get_frame_times(int64_t *times_us, int max, int64_t *now_us)
{
    unsigned int next = qatomic_load_acquire(&frame_times_next);
    /* Leave out the oldest few, which a new frame may be overwriting */
    int n = MIN(MIN(next, FRAME_TIMES - 8), (unsigned int)max);
    for (int i = 0; i < n; i++) {
        times_us[i] = frame_times[(next - n + i) % FRAME_TIMES];
    }
    *now_us = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
    return n;
}

const char *nv2a_profile_get_counter_name(unsigned int cnt)
{
    const char *default_names[NV2A_PROF__COUNT] = {
        #define _X(x) stringify(x),
        NV2A_PROF_COUNTERS_XMAC
        #undef _X
    };

    assert(cnt < NV2A_PROF__COUNT);
    return default_names[cnt] + 10; /* 'NV2A_PROF_' */
}

int nv2a_profile_get_counter_value(unsigned int cnt)
{
    assert(cnt < NV2A_PROF__COUNT);
    unsigned int idx = (g_nv2a_stats.frame_ptr + NV2A_PROF_NUM_FRAMES - 1) %
                       NV2A_PROF_NUM_FRAMES;
    return g_nv2a_stats.frame_history[idx].counters[cnt];
}
