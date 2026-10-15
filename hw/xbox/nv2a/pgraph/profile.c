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

static const char *const perf_names[XEMU_PERF__COUNT] = {
    [XEMU_PERF_SHADER_GEN] = "shader_new",
    [XEMU_PERF_SHADER_LOAD] = "shader_cached",
    [XEMU_PERF_AUDIO_VP] = "audio_voices",
    [XEMU_PERF_AUDIO_DSP] = "audio_dsp",
    [XEMU_PERF_TCG] = "code_translate",
    [XEMU_PERF_SHADER_ASYNC] = "shader_async",
    [XEMU_PERF_DRAW_SKIPPED] = "draw_skipped",
};

/* " name=count/total_ms(max_ms)" for each kind of work that happened */
static void perf_print_work(FILE *log, const int64_t *count, const int64_t *ns,
                            const int64_t *max_ns)
{
    for (int i = 0; i < XEMU_PERF__COUNT; i++) {
        if (count[i]) {
            fprintf(log, " %s=%" PRId64 "/%.1fms(max %.1f)", perf_names[i],
                    count[i], ns[i] / 1e6, max_ns[i] / 1e6);
        }
    }
}

/* The likely cause of a slow frame, from the work done during it */
static XemuDiagCause perf_cause(int64_t gap_us, const int64_t *ns)
{
    const int *counters = g_nv2a_stats.frame_working.counters;

    if (gap_us >= 2000000) {
        return XEMU_DIAG_CAUSE_PAUSE;
    }
    if (ns[XEMU_PERF_SHADER_GEN] * 3 >= gap_us * 1000) {
        return XEMU_DIAG_CAUSE_SHADER;
    }
    if (ns[XEMU_PERF_TCG] * 3 >= gap_us * 1000) {
        return XEMU_DIAG_CAUSE_CODE;
    }
    if (counters[NV2A_PROF_BEGIN_ENDS] <= 1) {
        return XEMU_DIAG_CAUSE_LOADING;
    }
    if (counters[NV2A_PROF_SURF_DOWNLOAD]) {
        return XEMU_DIAG_CAUSE_COPYBACK;
    }
    return XEMU_DIAG_CAUSE_OTHER;
}

/*
 * The frame lines of the diagnostics log (ui/xemu-diag.h), recorded from
 * XB Online > Diagnostics Log or, from the start, when the XEMU_PERF_LOG
 * environment variable names a file. One line is written per second with
 * the frame count, the longest gap between frames, CPU and memory use, the
 * work done and the per-second totals of every non-zero counter. Each frame
 * that took longer than PERF_SLOW_FRAME_US gets a "SLOW" line with its
 * likely cause and the work done during that frame alone.
 */
static void perf_log_frame(int64_t now)
{
    static bool env_checked;
    static int log_generation = -1;
    static int64_t window_start, last_frame;
    static int frames, slow_frames;
    static int64_t max_gap;
    static int totals[NV2A_PROF__COUNT];
    static int64_t sec_count[XEMU_PERF__COUNT], sec_ns[XEMU_PERF__COUNT],
                   sec_max[XEMU_PERF__COUNT];

    if (!env_checked) {
        const char *path = getenv("XEMU_PERF_LOG");
        env_checked = true;
        if (path && path[0]) {
            xemu_diag_start_path(path);
        }
    }

    int generation;
    int64_t start;
    FILE *log = xemu_diag_lock(&generation, &start);
    if (!log) {
        return;
    }
    if (generation != log_generation) {
        /* A new recording: start counting from here */
        log_generation = generation;
        window_start = last_frame = now;
        frames = slow_frames = 0;
        max_gap = 0;
        memset(totals, 0, sizeof(totals));
        memset(sec_count, 0, sizeof(sec_count));
        memset(sec_ns, 0, sizeof(sec_ns));
        memset(sec_max, 0, sizeof(sec_max));
        for (int i = 0; i < XEMU_PERF__COUNT; i++) {
            qatomic_set(&xemu_perf_count[i], 0);
            qatomic_set(&xemu_perf_ns[i], 0);
            qatomic_set(&xemu_perf_max_ns[i], 0);
        }
        xemu_diag_unlock();
        return;
    }

    /* Work done since the last frame */
    int64_t count[XEMU_PERF__COUNT], ns[XEMU_PERF__COUNT],
            max_ns[XEMU_PERF__COUNT];
    for (int i = 0; i < XEMU_PERF__COUNT; i++) {
        count[i] = qatomic_xchg(&xemu_perf_count[i], 0);
        ns[i] = qatomic_xchg(&xemu_perf_ns[i], 0);
        max_ns[i] = qatomic_xchg(&xemu_perf_max_ns[i], 0);
        sec_count[i] += count[i];
        sec_ns[i] += ns[i];
        sec_max[i] = MAX(sec_max[i], max_ns[i]);
    }

    int64_t gap = now - last_frame;
    bool slow = gap >= PERF_SLOW_FRAME_US;
    frames++;
    max_gap = MAX(max_gap, gap);
    last_frame = now;
    for (int i = 0; i < NV2A_PROF__COUNT; i++) {
        totals[i] += g_nv2a_stats.frame_working.counters[i];
    }

    XemuDiagCause cause = slow ? perf_cause(gap, ns) : XEMU_DIAG_CAUSE_OTHER;
    xemu_diag_count_frame(now, gap, slow, cause);
    if (slow) {
        slow_frames++;
        fprintf(log, "SLOW t=%.2f frame_ms=%.1f cause=%s", (now - start) / 1e6,
                gap / 1e3, xemu_diag_cause_name(cause));
        perf_print_work(log, count, ns, max_ns);
        for (int i = 0; i < NV2A_PROF__COUNT; i++) {
            if (g_nv2a_stats.frame_working.counters[i]) {
                fprintf(log, " %s=%d", nv2a_profile_get_counter_name(i),
                        g_nv2a_stats.frame_working.counters[i]);
            }
        }
        fputc('\n', log);
    }

    if (now - window_start >= 1000000) {
        fprintf(log, "t=%d frames=%d slow=%d max_gap_ms=%d",
                (int)((now - start) / 1000000), frames, slow_frames,
                (int)(max_gap / 1000));
        xemu_diag_print_system(log);
        perf_print_work(log, sec_count, sec_ns, sec_max);
        for (int i = 0; i < NV2A_PROF__COUNT; i++) {
            if (totals[i]) {
                fprintf(log, " %s=%d", nv2a_profile_get_counter_name(i),
                        totals[i]);
            }
        }
        fputc('\n', log);
        fflush(log);
        xemu_diag_count_second(frames);

        window_start = now;
        frames = 0;
        slow_frames = 0;
        max_gap = 0;
        memset(totals, 0, sizeof(totals));
        memset(sec_count, 0, sizeof(sec_count));
        memset(sec_ns, 0, sizeof(sec_ns));
        memset(sec_max, 0, sizeof(sec_max));
    }
    xemu_diag_unlock();
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
