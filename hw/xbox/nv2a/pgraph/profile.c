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

NV2AStats g_nv2a_stats;

/*
 * Optional performance log. When the XEMU_PERF_LOG environment variable names
 * a file, one line is appended per second with the frame count, the longest
 * gap between frames, and the per-second totals of every non-zero counter.
 */
static void perf_log_frame(int64_t now)
{
    static FILE *log;
    static bool initialized;
    static int64_t start, window_start, last_frame;
    static int frames;
    static int64_t max_gap;
    static int totals[NV2A_PROF__COUNT];

    if (!initialized) {
        const char *path = getenv("XEMU_PERF_LOG");
        initialized = true;
        if (path && path[0]) {
            log = fopen(path, "w");
        }
        start = window_start = last_frame = now;
    }
    if (!log) {
        return;
    }

    frames++;
    max_gap = MAX(max_gap, now - last_frame);
    last_frame = now;
    for (int i = 0; i < NV2A_PROF__COUNT; i++) {
        totals[i] += g_nv2a_stats.frame_working.counters[i];
    }

    if (now - window_start < 1000000) {
        return;
    }

    fprintf(log, "t=%d frames=%d max_gap_ms=%d", (int)((now - start) / 1000000),
            frames, (int)(max_gap / 1000));
    for (int i = 0; i < NV2A_PROF__COUNT; i++) {
        if (totals[i]) {
            fprintf(log, " %s=%d", nv2a_profile_get_counter_name(i), totals[i]);
        }
    }
    fputc('\n', log);
    fflush(log);

    window_start = now;
    frames = 0;
    max_gap = 0;
    memset(totals, 0, sizeof(totals));
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

    g_nv2a_stats.frame_working.mspf = render_time;
    g_nv2a_stats.frame_history[g_nv2a_stats.frame_ptr] =
        g_nv2a_stats.frame_working;
    g_nv2a_stats.frame_ptr =
        (g_nv2a_stats.frame_ptr + 1) % NV2A_PROF_NUM_FRAMES;
    g_nv2a_stats.frame_count++;
    memset(&g_nv2a_stats.frame_working, 0, sizeof(g_nv2a_stats.frame_working));
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
