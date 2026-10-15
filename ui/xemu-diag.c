/*
 * xemu diagnostics log
 *
 * See xemu-diag.h. The frame logger (hw/xbox/nv2a/pgraph/profile.c) reports
 * every frame from the render thread; a one second timer on the main loop
 * turns those into the per-second lines, so they go on while the game shows
 * no frames at all, and watches for freezes. Events come from the UI thread,
 * the connection check and the shader threads, so the lines and the counts
 * are kept under one lock.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qemu/atomic.h"
#include "qemu/bswap.h"
#include "qemu/timer.h"
#include "qemu/xemu-perf.h"
#include "qemu/main-loop.h"
#include "block/aio.h"
#include "block/accounting.h"
#include "system/block-backend.h"
#include "system/runstate.h"
#include "hw/core/cpu.h"
#include <SDL3/SDL.h>
#include <glib/gstdio.h>
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif

#include "xemu-diag.h"
#include "xemu-diag-threads.h"
#include "xemu-settings.h"
#include "xemu-os-utils.h"
#include "xemu-xbcheck.h"
#include "xemu-version.h"
#include "hw/xbox/nv2a/nv2a.h"
#include "hw/xbox/nv2a/debug.h"
#include "hw/xbox/nv2a/shader-precache.h"
#include "hw/xbox/mcpx/apu/apu_debug.h"

#define LOGS_DIR "logs"

/* The worst frames listed in the summary */
#define WORST_FRAMES 10

/* A second with fewer frames than this counts as a low second */
#define LOW_FPS 25

/* Lines kept in memory for a freeze report, and how far back they go */
#define RECENT_LINES 240
#define RECENT_SECONDS 60

/*
 * A freeze: no new frame for FREEZE_SECONDS while the emulator runs, with
 * the disc and hard disk reading less than FREEZE_LOADING_BYTES a second
 * for FREEZE_IDLE_IO_SECONDS (more is a long load; a game that streams its
 * music keeps reading a little), or no frame at all for
 * FREEZE_ANYWAY_SECONDS
 */
#define FREEZE_SECONDS 10
#define FREEZE_IDLE_IO_SECONDS 3
#define FREEZE_LOADING_BYTES (1024 * 1024)
#define FREEZE_ANYWAY_SECONDS 20

/*
 * Seconds with frames before the watch starts, so a screen that never
 * moves (some dashboards right after boot) does not count as a freeze
 */
#define FREEZE_ARM_SECONDS 10

/* Freeze reports kept in the logs folder; older ones are deleted */
#define FREEZE_REPORTS_KEPT 5

/* CPU samples kept while no frames come */
#define CPU_SAMPLES 32

/*
 * A slow frame with no other cause, while the emulated CPU's thread was
 * busy this share of it, is the game's own CPU work
 */
#define GAME_CPU_BUSY 0.9

/* Other programs on the PC are looked at this often (seconds) */
#define PROGRAMS_SECONDS 10

typedef struct WorstFrame {
    double t;
    double ms;
    XemuDiagCause cause;
} WorstFrame;

typedef struct RecentLine {
    int64_t us;
    const char *kind;   /* "SLOW", "EVENT", "MARK", or NULL for a second */
    char *text;
} RecentLine;

typedef struct CpuSample {
    int second;         /* of the freeze */
    bool ok;
    bool halted;
    uint32_t eip, esp, eflags, interrupt_request;
} CpuSample;

static GMutex lock;
static FILE *log_file;
static bool active;
static int64_t start_us;
static char *log_path;

/* A log is recording or freeze reports are on */
static int capturing;
static int tick_running;
static QEMUTimer *tick_timer;

/* The current second, filled by xemu_diag_frame */
static struct {
    int frames, slow;
    int64_t max_gap;
    int64_t count[XEMU_PERF__COUNT], ns[XEMU_PERF__COUNT],
            max_ns[XEMU_PERF__COUNT];
    int gpu[NV2A_PROF__COUNT];
} sec;
static int64_t last_frame_us;
static bool frames_seen;

/*
 * XEMU_FREEZE_TEST=S: frames from S to S + FREEZE_TEST_SECONDS seconds
 * after the first one are not counted, to try out freeze reports
 */
#define FREEZE_TEST_SECONDS 20
static int freeze_test_s;
static int64_t first_frame_us;

static RecentLine recent[RECENT_LINES];
static int recent_next;

/* Summary */
static int64_t frame_count;
static int64_t slow_count[XEMU_DIAG_CAUSE__COUNT];
static double slow_ms[XEMU_DIAG_CAUSE__COUNT];
static int second_count, low_second_count, mark_count;
static WorstFrame worst[WORST_FRAMES];
static int worst_count;

/* Freeze watch, on the main loop only */
static int frame_seconds, no_frame_seconds, io_idle_seconds;
static bool freeze_noticed;
static CpuSample cpu_samples[CPU_SAMPLES];
static int cpu_sample_count;
static int64_t freeze_us = -1;

/* Watched by xemu_diag_poll */
static bool watching;
static bool had_focus;
static char *last_disc;
static bool cache_reported;

static const char *const cause_names[XEMU_DIAG_CAUSE__COUNT] = {
    [XEMU_DIAG_CAUSE_SHADER] = "new_shader",
    [XEMU_DIAG_CAUSE_COPYBACK] = "screen_copyback",
    [XEMU_DIAG_CAUSE_CODE] = "code_translate",
    [XEMU_DIAG_CAUSE_LOADING] = "loading_screen",
    [XEMU_DIAG_CAUSE_PAUSE] = "no_frames",
    [XEMU_DIAG_CAUSE_QUERY] = "query_wait",
    [XEMU_DIAG_CAUSE_AUDIO] = "audio_wait",
    [XEMU_DIAG_CAUSE_GAME_CPU] = "game_cpu",
    [XEMU_DIAG_CAUSE_OTHER] = "other",
};

static const char *const cause_text[XEMU_DIAG_CAUSE__COUNT] = {
    [XEMU_DIAG_CAUSE_SHADER] = "building a new shader",
    [XEMU_DIAG_CAUSE_COPYBACK] = "copying the screen back from the GPU",
    [XEMU_DIAG_CAUSE_CODE] = "translating game code",
    [XEMU_DIAG_CAUSE_LOADING] = "loading screen",
    [XEMU_DIAG_CAUSE_PAUSE] = "no frames for 2 s or more (paused, menu, "
                              "window moved)",
    [XEMU_DIAG_CAUSE_QUERY] = "waiting for the GPU to count visible pixels",
    [XEMU_DIAG_CAUSE_AUDIO] = "the game waiting for the sound chip",
    [XEMU_DIAG_CAUSE_GAME_CPU] = "the emulated Xbox CPU busy the whole frame",
    [XEMU_DIAG_CAUSE_OTHER] = "other",
};

static const char *const perf_names[XEMU_PERF__COUNT] = {
    [XEMU_PERF_SHADER_GEN] = "shader_new",
    [XEMU_PERF_SHADER_LOAD] = "shader_cached",
    [XEMU_PERF_AUDIO_VP] = "audio_voices",
    [XEMU_PERF_AUDIO_DSP] = "audio_dsp",
    [XEMU_PERF_TCG] = "code_translate",
    [XEMU_PERF_SHADER_ASYNC] = "shader_async",
    [XEMU_PERF_DRAW_SKIPPED] = "draw_skipped",
    [XEMU_PERF_COPYBACK] = "copyback",
    [XEMU_PERF_SURF_UPLOAD] = "surface_upload",
    [XEMU_PERF_QUERY_WAIT] = "query_wait",
    [XEMU_PERF_AUDIO_LOCK] = "audio_lock_wait",
    [XEMU_PERF_AUDIO_DROPOUT] = "audio_dropouts",
    [XEMU_PERF_BQL_WAIT] = "main_lock_wait",
    [XEMU_PERF_CODE_WRITE] = "code_page_write",
    [XEMU_PERF_CODE_DROPPED] = "code_dropped",
    [XEMU_PERF_CODE_DMA] = "code_dma_write",
    [XEMU_PERF_CODE_FLUSH] = "code_flush",
    [XEMU_PERF_IO_RECOMPILE] = "io_recompile",
};

const char *xemu_diag_cause_name(XemuDiagCause cause)
{
    return cause_names[cause];
}

static int64_t now_us(void)
{
    return qemu_clock_get_us(QEMU_CLOCK_REALTIME);
}

static double elapsed(void)
{
    return (now_us() - start_us) / 1e6;
}

static char *logs_dir(void)
{
    return g_build_filename(xemu_settings_get_base_path(), LOGS_DIR, NULL);
}

static const char *base_name(const char *path)
{
    const char *slash = strrchr(path, '/');
    const char *back = strrchr(path, '\\');
    if (back > slash) {
        slash = back;
    }
    return slash ? slash + 1 : path;
}

/* The lines of the package's version.txt worth knowing */
static void write_package(FILE *f)
{
    const char *exe_dir = SDL_GetBasePath();
    char *path = g_build_filename(exe_dir ? exe_dir : ".", "version.txt", NULL);
    char *text = NULL;
    if (g_file_get_contents(path, &text, NULL, NULL)) {
        char **lines = g_strsplit(text, "\n", -1);
        fprintf(f, "# package:");
        for (char **l = lines; *l; l++) {
            g_strstrip(*l);
            if (g_str_has_prefix(*l, "build=") ||
                g_str_has_prefix(*l, "version=") ||
                g_str_has_prefix(*l, "date=")) {
                fprintf(f, " %s", *l);
            }
        }
        fputc('\n', f);
        g_strfreev(lines);
    } else {
        fprintf(f, "# package: no version.txt\n");
    }
    g_free(text);
    g_free(path);
}

static void write_machine(FILE *f)
{
    fprintf(f, "# os: %s\n", xemu_get_os_info());
#ifdef _WIN32
    char cpu[128] = "";
    DWORD size = sizeof(cpu);
    if (RegGetValueA(HKEY_LOCAL_MACHINE,
                     "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                     "ProcessorNameString", RRF_RT_REG_SZ, NULL, cpu,
                     &size) != ERROR_SUCCESS) {
        cpu[0] = '\0';
    }
    fprintf(f, "# cpu: %s (%d threads)\n", g_strstrip(cpu),
            g_get_num_processors());
    MEMORYSTATUSEX mem = { .dwLength = sizeof(mem) };
    if (GlobalMemoryStatusEx(&mem)) {
        fprintf(f, "# ram: %llu MB, %llu MB free\n",
                (unsigned long long)(mem.ullTotalPhys >> 20),
                (unsigned long long)(mem.ullAvailPhys >> 20));
    }
#else
    fprintf(f, "# cpu: %d threads\n", g_get_num_processors());
#endif

    ShaderPrecacheStatus status;
    nv2a_shader_precache_get_status(&status);
    if (status.gpu[0]) {
        fprintf(f, "# gpu: %s\n# driver: %s\n", status.gpu, status.driver);
    }
}

static void write_settings(FILE *f)
{
    static const char *const renderers[] = { "null", "opengl", "vulkan" };
    static const char *const backends[] = { "nat", "udp", "pcap" };
    static const char *const redirects[] = { "none", "insignia", "xb_online",
                                             "custom" };
    int renderer = g_config.display.renderer;
    int backend = g_config.net.backend;
    int redirect = g_config.net.dns_redirect;

    fprintf(f, "# settings: renderer=%s surface_scale=%d vsync=%d "
            "skip_reuse_copyback=%d use_dsp=%d use_dsp_jit=%d hrtf=%d "
            "voice_workers=%d hard_fpu=%d "
            "cache_shaders=%d shader_async=%d precache_auto=%d "
            "precache_own=%d\n",
            renderer < 3 ? renderers[renderer] : "?",
            g_config.display.quality.surface_scale,
            g_config.display.window.vsync,
            g_config.display.quality.skip_reuse_copyback,
            g_config.audio.use_dsp, g_config.audio.use_dsp_jit,
            g_config.audio.hrtf,
            /* 0 in the settings means auto, as vp.c does */
            g_config.audio.vp.num_workers ?:
                MIN(AUTO_VOICE_WORKERS, SDL_GetNumLogicalCPUCores()),
            g_config.perf.hard_fpu, g_config.perf.cache_shaders,
            g_config.perf.shader_async, g_config.perf.shader_precache_auto,
            g_config.perf.shader_precache_own);
    fprintf(f, "# network: enabled=%d backend=%s dns_redirect=%s\n",
            g_config.net.enable, backend < 3 ? backends[backend] : "?",
            redirect < 4 ? redirects[redirect] : "?");
    const char *disc = g_config.sys.files.dvd_path;
    fprintf(f, "# disc: %s\n", disc && disc[0] ? base_name(disc) : "none");
}

static void write_header(FILE *f, const char *title)
{
    GDateTime *now = g_date_time_new_now_local();
    char *when = g_date_time_format(now, "%Y-%m-%d %H:%M:%S");
    fprintf(f, "# Black Arrow Online %s %s\n", title, when);
    g_free(when);
    g_date_time_unref(now);

    write_package(f);
    fprintf(f, "# xemu: %s (%.10s, %s)\n", xemu_version, xemu_commit,
            xemu_date);
    write_machine(f);
    write_settings(f);
}

static const char lines_help[] =
    "#   t=SECOND  one per second: frames shown, slow frames, longest gap, "
    "disc and hard disk reads (MB/time spent), cpu (xemu, whole PC), "
    "memory, thread_cpu (% of one core per emulator thread: game_cpu is "
    "the emulated Xbox CPU, gpu the graphics thread, ui the window, audio "
    "the sound chip, voices its mixing helpers), work done (count/total "
    "ms), GPU counters\n"
    "#   SLOW      a frame of 50 ms or more, with its likely cause, how "
    "busy the emulated CPU was (game_cpu) and the work done during it\n"
    "#   MARK      the player pressed F9\n"
    "#   EVENT     focus, disc, connection check, shader cache, freeze, "
    "other programs using the PC\n";

/* " name=count/total_ms(max_ms)" for each kind of work that happened */
static void print_work(GString *s, const int64_t *count, const int64_t *ns,
                       const int64_t *max_ns)
{
    for (int i = 0; i < XEMU_PERF__COUNT; i++) {
        bool count_only = i == XEMU_PERF_AUDIO_DROPOUT ||
                          i == XEMU_PERF_CODE_DROPPED ||
                          i == XEMU_PERF_CODE_DMA ||
                          i == XEMU_PERF_CODE_FLUSH ||
                          i == XEMU_PERF_IO_RECOMPILE;
        if (count[i] && count_only) {
            g_string_append_printf(s, " %s=%" PRId64, perf_names[i],
                                   count[i]);
        } else if (count[i]) {
            g_string_append_printf(s, " %s=%" PRId64 "/%.1fms(max %.1f)",
                                   perf_names[i], count[i], ns[i] / 1e6,
                                   max_ns[i] / 1e6);
        }
    }
}

static void print_gpu(GString *s, const int *gpu)
{
    for (int i = 0; i < NV2A_PROF__COUNT; i++) {
        if (gpu[i]) {
            g_string_append_printf(s, " %s=%d",
                                   nv2a_profile_get_counter_name(i), gpu[i]);
        }
    }
}

/* A line: into memory for freeze reports, and into the log if recording */
static void add_line(int64_t us, const char *kind, const char *text)
{
    g_mutex_lock(&lock);
    if (log_file) {
        if (kind) {
            fprintf(log_file, "%s t=%.2f %s\n", kind, (us - start_us) / 1e6,
                    text);
        } else {
            fprintf(log_file, "t=%d %s\n", (int)((us - start_us) / 1000000),
                    text);
        }
        if (!kind || strcmp(kind, "SLOW")) {
            fflush(log_file);
        }
    }
    RecentLine *l = &recent[recent_next];
    g_free(l->text);
    *l = (RecentLine){ us, kind, g_strdup(text) };
    recent_next = (recent_next + 1) % RECENT_LINES;
    g_mutex_unlock(&lock);
}

/*
 * Game code translated and code pages written while recording, by address,
 * so the summary can show which code keeps being thrown away. Only the
 * game CPU thread adds; a full table drops new addresses.
 */
#define CODE_SLOTS 4096

typedef struct CodeSlot {
    uint64_t key, page;
    uint32_t count;
} CodeSlot;

static CodeSlot code_translated[CODE_SLOTS], code_written[CODE_SLOTS];

static void code_count(CodeSlot *table, uint64_t key, uint64_t page)
{
    uint32_t h = (key * 0x9E3779B97F4A7C15ull) >> 52;
    for (int i = 0; i < 32; i++) {
        CodeSlot *slot = &table[(h + i) & (CODE_SLOTS - 1)];
        if (!slot->count) {
            slot->key = key;
            slot->page = page;
            slot->count = 1;
            return;
        }
        if (slot->key == key) {
            slot->count++;
            return;
        }
    }
}

void xemu_perf_code_translated(uint64_t pc, uint64_t ram_page)
{
    code_count(code_translated, pc, ram_page);
}

void xemu_perf_code_written(uint64_t ram_page)
{
    code_count(code_written, ram_page, ram_page);
}

static int code_slot_cmp(const void *a, const void *b)
{
    const CodeSlot *x = a, *y = b;
    return x->count < y->count ? 1 : x->count > y->count ? -1 : 0;
}

/* The 12 most frequent entries of a table */
static void print_code_top(FILE *f, const char *title, CodeSlot *table,
                           bool with_page)
{
    CodeSlot *sorted = g_memdup2(table, sizeof(CodeSlot) * CODE_SLOTS);
    qsort(sorted, CODE_SLOTS, sizeof(CodeSlot), code_slot_cmp);
    uint64_t total = 0;
    int used = 0;
    for (int i = 0; i < CODE_SLOTS; i++) {
        total += sorted[i].count;
        used += sorted[i].count > 0;
    }
    if (total) {
        fprintf(f, "SUMMARY %s: %" PRIu64 " at %d addresses, most:", title,
                total, used);
        for (int i = 0; i < 12 && sorted[i].count; i++) {
            if (with_page) {
                fprintf(f, " 0x%" PRIx64 "(page 0x%" PRIx64 "):%u",
                        sorted[i].key, sorted[i].page, sorted[i].count);
            } else {
                fprintf(f, " page 0x%" PRIx64 ":%u", sorted[i].key,
                        sorted[i].count);
            }
        }
        fputc('\n', f);
    }
    g_free(sorted);
}

static void reset_summary(void)
{
    memset(code_translated, 0, sizeof(code_translated));
    memset(code_written, 0, sizeof(code_written));
    frame_count = 0;
    memset(slow_count, 0, sizeof(slow_count));
    memset(slow_ms, 0, sizeof(slow_ms));
    second_count = low_second_count = mark_count = 0;
    worst_count = 0;
}

/* With the lock held */
static void write_summary(void)
{
    double secs = elapsed();
    int64_t slow_total = 0;
    for (int i = 0; i < XEMU_DIAG_CAUSE__COUNT; i++) {
        slow_total += slow_count[i];
    }

    /* Leave out the stretches with no frames at all */
    double running = secs - slow_ms[XEMU_DIAG_CAUSE_PAUSE] / 1e3;
    fprintf(log_file, "SUMMARY recorded %.0f s, %" PRId64 " frames, "
            "average %.1f fps while running\n", secs, frame_count,
            running > 0 ? frame_count / running : 0.0);
    fprintf(log_file, "SUMMARY seconds under %d fps: %d of %d\n", LOW_FPS,
            low_second_count, second_count);
    fprintf(log_file, "SUMMARY slow frames: %" PRId64 "\n", slow_total);
    for (int i = 0; i < XEMU_DIAG_CAUSE__COUNT; i++) {
        if (slow_count[i]) {
            fprintf(log_file, "SUMMARY   %s: %" PRId64 " frames, %.0f ms\n",
                    cause_text[i], slow_count[i], slow_ms[i]);
        }
    }
    fprintf(log_file, "SUMMARY markers: %d\n", mark_count);
    print_code_top(log_file, "code translated", code_translated, true);
    print_code_top(log_file, "code pages written", code_written, false);
    GString *usage = g_string_new(NULL);
    xemu_diag_threads_summary(usage);
    xemu_diag_programs_summary(usage);
    fputs(usage->str, log_file);
    g_string_free(usage, true);
    for (int i = 0; i < worst_count; i++) {
        fprintf(log_file, "SUMMARY worst %d: t=%.2f %.0f ms (%s)\n", i + 1,
                worst[i].t, worst[i].ms, cause_names[worst[i].cause]);
    }
    fflush(log_file);
}

static void at_exit(void)
{
    xemu_diag_stop();
}

static void tick_start_bh(void *opaque);

/* Start the one second timer if it is not running */
static void start_tick(void)
{
    AioContext *ctx = qemu_get_aio_context();
    if (ctx && !qatomic_cmpxchg(&tick_running, 0, 1)) {
        aio_bh_schedule_oneshot(ctx, tick_start_bh, NULL);
    }
}

bool xemu_diag_start_path(const char *path)
{
    static bool exit_registered;

    xemu_diag_stop();

    FILE *f = g_fopen(path, "w");
    if (!f) {
        fprintf(stderr, "diag: cannot write %s\n", path);
        return false;
    }

    /* Count the shaders on disk so the poll can report them */
    nv2a_shader_precache_scan();
    write_header(f, "diagnostics log, started");
    fprintf(f, "#\n# Lines:\n%s#   SUMMARY   when recording stops\n#\n",
            lines_help);
    fflush(f);

    xemu_diag_threads_reset();
    g_mutex_lock(&lock);
    log_file = f;
    g_free(log_path);
    log_path = g_strdup(path);
    start_us = now_us();
    reset_summary();
    cache_reported = false;
    qatomic_set(&active, true);
    g_mutex_unlock(&lock);

    qatomic_set(&capturing, 1);
    qatomic_set(&xemu_perf_enabled, true);
    start_tick();
    xemu_diag_write_check();

    if (!exit_registered) {
        atexit(at_exit);
        exit_registered = true;
    }
    return true;
}

static char *dated_name(const char *prefix)
{
    GDateTime *now = g_date_time_new_now_local();
    char *stamp = g_date_time_format(now, "%Y-%m-%d_%H-%M-%S");
    char *name = g_strdup_printf("%s-%s.txt", prefix, stamp);
    g_free(stamp);
    g_date_time_unref(now);
    return name;
}

bool xemu_diag_start(void)
{
    char *dir = logs_dir();
    g_mkdir_with_parents(dir, 0755);

    char *name = dated_name("diagnostics");
    char *path = g_build_filename(dir, name, NULL);
    bool ok = xemu_diag_start_path(path);

    g_free(path);
    g_free(name);
    g_free(dir);
    return ok;
}

void xemu_diag_stop(void)
{
    g_mutex_lock(&lock);
    if (log_file) {
        write_summary();
        fclose(log_file);
        log_file = NULL;
    }
    qatomic_set(&active, false);
    g_mutex_unlock(&lock);
}

bool xemu_diag_active(void)
{
    return qatomic_read(&active);
}

bool xemu_diag_recording(void)
{
    return qatomic_read(&capturing);
}

double xemu_diag_seconds(void)
{
    return xemu_diag_active() ? elapsed() : 0;
}

const char *xemu_diag_path(void)
{
    return log_path;
}

static void open_path(const char *file)
{
    char *path = g_canonicalize_filename(file, NULL);
#ifdef _WIN32
    /* Explorer opens a folder or a file given as a plain path */
    char *url = g_strdup(path);
#else
    char *url = g_filename_to_uri(path, NULL, NULL);
#endif
    if (url) {
        SDL_OpenURL(url);
    }
    g_free(url);
    g_free(path);
}

void xemu_diag_open_folder(void)
{
    char *dir = logs_dir();
    g_mkdir_with_parents(dir, 0755);
    open_path(dir);
    g_free(dir);
}

void xemu_diag_mark(void)
{
    if (!xemu_diag_active()) {
        return;
    }
    char text[32];
    g_mutex_lock(&lock);
    snprintf(text, sizeof(text), "n=%d", ++mark_count);
    g_mutex_unlock(&lock);
    add_line(now_us(), "MARK", text);
}

void xemu_diag_event(const char *fmt, ...)
{
    if (!xemu_diag_recording()) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    char *text = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    add_line(now_us(), "EVENT", text);
    g_free(text);
}

void xemu_diag_write_check(void)
{
    static const char *const states[] = { "ok", "info", "warn", "fail" };
    XbCheckItem items[XBCHECK_MAX_ITEMS];
    bool running;

    if (!xemu_diag_recording()) {
        return;
    }
    int n = xemu_xbcheck_get(items, XBCHECK_MAX_ITEMS, &running);
    if (running || n == 0) {
        return;
    }
    for (int i = 0; i < n; i++) {
        int state = items[i].state;
        xemu_diag_event("check %s: %s - %s", items[i].name,
                        state < 4 ? states[state] : "?", items[i].detail);
    }
}

void xemu_diag_poll(void)
{
    static bool env_checked;
    if (!env_checked) {
        const char *path = getenv("XEMU_PERF_LOG");
        env_checked = true;
        if (path && path[0]) {
            xemu_diag_start_path(path);
        }
        const char *test = getenv("XEMU_FREEZE_TEST");
        if (test && atoi(test) > 0) {
            qatomic_set(&freeze_test_s, atoi(test));
        }
    }

    static bool ui_marked;
    if (!ui_marked) {
        ui_marked = true;
        xemu_diag_threads_mark_ui();
    }

    bool capture = xemu_diag_active() || g_config.perf.freeze_report;
    qatomic_set(&capturing, capture);
    qatomic_set(&xemu_perf_enabled, capture);
    if (!capture) {
        watching = false;
        return;
    }
    start_tick();

    bool focus = SDL_GetKeyboardFocus() != NULL;
    const char *disc = g_config.sys.files.dvd_path;
    if (!watching) {
        watching = true;
        had_focus = focus;
        g_free(last_disc);
        last_disc = g_strdup(disc);
    }
    if (focus != had_focus) {
        had_focus = focus;
        xemu_diag_event("%s", focus ? "window got focus" : "window lost focus");
    }
    if (g_strcmp0(disc, last_disc)) {
        g_free(last_disc);
        last_disc = g_strdup(disc);
        xemu_diag_event("disc: %s", disc && disc[0] ? base_name(disc) :
                                                      "ejected");
    }

    if (!cache_reported) {
        ShaderPrecacheStatus status;
        nv2a_shader_precache_get_status(&status);
        if (!status.supported) {
            cache_reported = true;
        } else if (status.scanned && !status.running) {
            cache_reported = true;
            xemu_diag_event("shader cache: %d saved (%.1f MB), list %d, "
                            "%d still to build", status.disk_count,
                            status.disk_bytes / 1048576.0,
                            status.list_total, status.list_missing);
        }
    }
}

/* With the lock held */
static void count_frame(int64_t now, int64_t gap_us, bool slow,
                        XemuDiagCause cause)
{
    frame_count++;
    if (!slow) {
        return;
    }
    double ms = gap_us / 1e3;
    slow_count[cause]++;
    slow_ms[cause] += ms;

    /* Keep the worst frames, longest first */
    int i = worst_count < WORST_FRAMES ? worst_count++ : WORST_FRAMES;
    if (i == WORST_FRAMES) {
        if (ms <= worst[WORST_FRAMES - 1].ms) {
            return;
        }
        i = WORST_FRAMES - 1;
    }
    while (i > 0 && worst[i - 1].ms < ms) {
        worst[i] = worst[i - 1];
        i--;
    }
    worst[i] = (WorstFrame){ (now - start_us) / 1e6, ms, cause };
}

void xemu_diag_frame(int64_t now, int64_t gap_us, bool slow,
                     XemuDiagCause cause, const int64_t *count,
                     const int64_t *ns, const int64_t *max_ns, const int *gpu,
                     int gpu_n)
{
    int frame_gpu[NV2A_PROF__COUNT] = { 0 };
    memcpy(frame_gpu, gpu, MIN(gpu_n, NV2A_PROF__COUNT) * sizeof(int));

    int test = qatomic_read(&freeze_test_s);
    if (!first_frame_us) {
        first_frame_us = now;
    } else if (test && now - first_frame_us >= test * 1000000LL &&
               now - first_frame_us < (test + FREEZE_TEST_SECONDS) *
                                      1000000LL) {
        return;
    }

    /* Every frame, so the reading covers just this one */
    double game_cpu = xemu_diag_active() ? xemu_diag_threads_game_cpu() : -1;
    if (slow && cause == XEMU_DIAG_CAUSE_OTHER && game_cpu >= GAME_CPU_BUSY) {
        cause = XEMU_DIAG_CAUSE_GAME_CPU;
    }

    g_mutex_lock(&lock);
    sec.frames++;
    sec.max_gap = MAX(sec.max_gap, gap_us);
    for (int i = 0; i < XEMU_PERF__COUNT; i++) {
        sec.count[i] += count[i];
        sec.ns[i] += ns[i];
        sec.max_ns[i] = MAX(sec.max_ns[i], max_ns[i]);
    }
    for (int i = 0; i < NV2A_PROF__COUNT; i++) {
        sec.gpu[i] += frame_gpu[i];
    }
    last_frame_us = now;
    frames_seen = true;
    if (log_file) {
        count_frame(now, gap_us, slow, cause);
    }
    if (slow) {
        sec.slow++;
    }
    g_mutex_unlock(&lock);

    if (slow) {
        GString *s = g_string_new(NULL);
        g_string_append_printf(s, "frame_ms=%.1f cause=%s", gap_us / 1e3,
                               cause_names[cause]);
        if (game_cpu >= 0) {
            g_string_append_printf(s, " game_cpu=%.0f%%",
                                   MIN(game_cpu, 1.0) * 100);
        }
        print_work(s, count, ns, max_ns);
        print_gpu(s, frame_gpu);
        add_line(now, "SLOW", s->str);
        g_string_free(s, true);
    }
}

/*
 * Disc and hard disk reads. The block layer keeps byte and time totals per
 * drive, and may only be walked from the main loop, where the timer runs.
 * Appends " disc_read=MB/ms hdd_read=MB/ms hdd_write=MB" for the time since
 * the last call and returns the bytes read.
 */
enum { IO_HDD, IO_DISC, IO__COUNT };
enum { IO_READ_BYTES, IO_READ_NS, IO_WRITE_BYTES, IO__FIELDS };

static uint64_t io_last[IO__COUNT][IO__FIELDS];
static bool io_have_last;

static uint64_t print_io(GString *s)
{
    static const char *const names[IO__COUNT] = { "hdd", "disc" };
    uint64_t v[IO__COUNT][IO__FIELDS] = { 0 };
    uint64_t read = 0;

    for (BlockBackend *blk = blk_next(NULL); blk; blk = blk_next(blk)) {
        BlockAcctStats *stats = blk_get_stats(blk);
        int drive = strstr(blk_name(blk), "-cd") ? IO_DISC : IO_HDD;
        v[drive][IO_READ_BYTES] += stats->nr_bytes[BLOCK_ACCT_READ];
        v[drive][IO_READ_NS] += stats->total_time_ns[BLOCK_ACCT_READ];
        v[drive][IO_WRITE_BYTES] += stats->nr_bytes[BLOCK_ACCT_WRITE];
    }
    for (int d = IO__COUNT - 1; io_have_last && d >= 0; d--) {
        uint64_t rd = v[d][IO_READ_BYTES] - io_last[d][IO_READ_BYTES];
        uint64_t rd_ns = v[d][IO_READ_NS] - io_last[d][IO_READ_NS];
        uint64_t wr = v[d][IO_WRITE_BYTES] - io_last[d][IO_WRITE_BYTES];
        if (rd) {
            g_string_append_printf(s, " %s_read=%.1fMB/%.0fms", names[d],
                                   rd / 1048576.0, rd_ns / 1e6);
            read += rd;
        }
        if (wr) {
            g_string_append_printf(s, " %s_write=%.1fMB", names[d],
                                   wr / 1048576.0);
        }
    }
    memcpy(io_last, v, sizeof(v));
    io_have_last = true;
    return read;
}

#ifdef _WIN32
static ULONGLONG filetime(FILETIME ft)
{
    return ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}
#endif

/* " cpu=% sys_cpu=% mem_mb=" since the last call */
static void print_system(GString *s)
{
#ifdef _WIN32
    static ULONGLONG last_proc, last_busy, last_total, last_wall;
    FILETIME c, e, k, u, idle, kern, user;

    if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u) ||
        !GetSystemTimes(&idle, &kern, &user)) {
        return;
    }
    ULONGLONG proc = filetime(k) + filetime(u);
    /* Kernel time includes idle time */
    ULONGLONG total = filetime(kern) + filetime(user);
    ULONGLONG busy = total - filetime(idle);
    ULONGLONG wall = now_us() * 10;  /* 100 ns units, like the times */

    if (last_wall && wall > last_wall && total > last_total) {
        double cpus = g_get_num_processors();
        g_string_append_printf(s, " cpu=%.0f%% sys_cpu=%.0f%%",
            100.0 * (proc - last_proc) / ((wall - last_wall) * cpus),
            100.0 * (busy - last_busy) / (total - last_total));
    }
    last_proc = proc;
    last_busy = busy;
    last_total = total;
    last_wall = wall;

    PROCESS_MEMORY_COUNTERS mem = { .cb = sizeof(mem) };
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), &mem, sizeof(mem))) {
        g_string_append_printf(s, " mem_mb=%zu",
                               (size_t)(mem.WorkingSetSize >> 20));
    }
#endif
}

/* i386 registers, numbered as the GDB stub numbers them */
enum { REG_ESP = 4, REG_EIP = 8, REG_EFLAGS = 9 };

static uint32_t cpu_reg(CPUState *cs, int reg, bool *ok)
{
    CPUClass *cc = CPU_GET_CLASS(cs);
    GByteArray *buf = g_byte_array_new();
    uint32_t v = 0;
    if (cc->gdb_read_register && cc->gdb_read_register(cs, buf, reg) >= 4) {
        v = ldl_le_p(buf->data);
    } else {
        *ok = false;
    }
    g_byte_array_free(buf, true);
    return v;
}

/*
 * Where the Xbox CPU is. Read while it runs, which is good enough to see
 * whether it is stuck in one place or halted.
 */
static CpuSample sample_cpu(int second)
{
    CpuSample c = { .second = second, .ok = true };
    CPUState *cs = first_cpu;
    if (!cs) {
        c.ok = false;
        return c;
    }
    c.eip = cpu_reg(cs, REG_EIP, &c.ok);
    c.esp = cpu_reg(cs, REG_ESP, &c.ok);
    c.eflags = cpu_reg(cs, REG_EFLAGS, &c.ok);
    c.halted = qatomic_read(&cs->halted);
    c.interrupt_request = qatomic_read(&cs->interrupt_request);
    return c;
}

static void print_cpu_sample(FILE *f, const CpuSample *c, int now_second)
{
    if (!c->ok) {
        fprintf(f, "CPU t=%d not readable\n", c->second - now_second);
        return;
    }
    fprintf(f, "CPU t=%d eip=%08x (%s) esp=%08x eflags=%08x halted=%d "
            "interrupt_request=%x\n", c->second - now_second, c->eip,
            c->eip >= 0x80000000 ? "Xbox kernel" : "game", c->esp, c->eflags,
            c->halted, c->interrupt_request);
}

static void print_stack(FILE *f)
{
    CPUState *cs = first_cpu;
    bool ok = true;
    if (!cs) {
        return;
    }
    uint32_t esp = cpu_reg(cs, REG_ESP, &ok);
    uint32_t stack[16];
    if (!ok || cpu_memory_rw_debug(cs, esp, stack, sizeof(stack), false)) {
        fprintf(f, "STACK not readable\n");
        return;
    }
    for (int i = 0; i < 16; i += 8) {
        fprintf(f, "STACK %08x:", esp + i * 4);
        for (int j = i; j < i + 8; j++) {
            fprintf(f, " %08x", le32_to_cpu(stack[j]));
        }
        fputc('\n', f);
    }
}

static gint compare_paths(gconstpointer a, gconstpointer b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* The freeze reports in the logs folder, oldest first */
static GPtrArray *freeze_reports(void)
{
    GPtrArray *paths = g_ptr_array_new_with_free_func(g_free);
    char *dir = logs_dir();
    GDir *d = g_dir_open(dir, 0, NULL);
    if (d) {
        const char *name;
        while ((name = g_dir_read_name(d))) {
            if (g_str_has_prefix(name, "freeze-") &&
                g_str_has_suffix(name, ".txt")) {
                g_ptr_array_add(paths, g_build_filename(dir, name, NULL));
            }
        }
        g_dir_close(d);
    }
    g_free(dir);
    /* The names carry the date and time, so they sort by age */
    g_ptr_array_sort(paths, compare_paths);
    return paths;
}

static void prune_freeze_reports(void)
{
    GPtrArray *paths = freeze_reports();
    for (int i = 0; i + FREEZE_REPORTS_KEPT < (int)paths->len; i++) {
        g_remove(g_ptr_array_index(paths, i));
    }
    g_ptr_array_unref(paths);
}

char *xemu_diag_last_freeze_report(void)
{
    GPtrArray *paths = freeze_reports();
    char *path = paths->len ?
                 g_strdup(g_ptr_array_index(paths, paths->len - 1)) : NULL;
    g_ptr_array_unref(paths);
    return path;
}

bool xemu_diag_open_freeze_report(void)
{
    char *path = xemu_diag_last_freeze_report();
    if (!path) {
        return false;
    }
    open_path(path);
    g_free(path);
    return true;
}

double xemu_diag_freeze_age(void)
{
    int64_t t = qatomic_read(&freeze_us);
    return t < 0 ? -1 : (now_us() - t) / 1e6;
}

/* Write a freeze report; returns its file name, or NULL */
static char *write_freeze_report(int64_t now)
{
    char *dir = logs_dir();
    g_mkdir_with_parents(dir, 0755);
    char *name = dated_name("freeze");
    char *path = g_build_filename(dir, name, NULL);
    FILE *f = g_fopen(path, "w");
    g_free(path);
    g_free(dir);
    if (!f) {
        fprintf(stderr, "diag: cannot write the freeze report %s\n", name);
        g_free(name);
        return NULL;
    }

    write_header(f, "freeze report,");
    fprintf(f,
        "#\n"
        "# The game showed no new picture for %d s while the emulator kept "
        "running.\n"
        "#   CPU       the Xbox CPU once a second since the last picture: "
        "where it was (game code, or the Xbox kernel) and whether it was "
        "halted, waiting for an interrupt\n"
        "#   STACK     the top of the Xbox CPU's stack now\n"
        "#   GPU       the graphics chip: commands queued (dma_put) and read "
        "(dma_get), what it waits for, interrupts (pending/enabled)\n"
        "#   SHADERS   background shader building\n"
        "# Then the diagnostics lines of the last minute, t in seconds "
        "before this report:\n%s#\n", no_frame_seconds, lines_help);

    fprintf(f, "FREEZE no_picture_s=%d disc_hdd_idle_s=%d "
            "last_frame_s=%.1f\n", no_frame_seconds, io_idle_seconds,
            (now - last_frame_us) / 1e6);
    for (int i = 0; i < cpu_sample_count; i++) {
        print_cpu_sample(f, &cpu_samples[i], no_frame_seconds);
    }
    print_stack(f);

    GString *gpu = g_string_new(NULL);
    nv2a_freeze_state(gpu);
    fputs(gpu->str, f);
    g_string_free(gpu, true);

    ShaderPrecacheStatus status;
    nv2a_shader_precache_get_status(&status);
    fprintf(f, "SHADERS background=%d precache=%s built=%d failed=%d\n",
            g_config.perf.shader_async, status.running ? "running" : "idle",
            status.done, status.failed);

    g_mutex_lock(&lock);
    for (int i = 0; i < RECENT_LINES; i++) {
        RecentLine *l = &recent[(recent_next + i) % RECENT_LINES];
        if (!l->text || now - l->us > RECENT_SECONDS * 1000000LL) {
            continue;
        }
        double t = (l->us - now) / 1e6;
        if (l->kind) {
            fprintf(f, "%s t=%.2f %s\n", l->kind, t, l->text);
        } else {
            fprintf(f, "t=%.0f %s\n", t, l->text);
        }
    }
    g_mutex_unlock(&lock);
    fclose(f);

    prune_freeze_reports();
    return name;
}

/* Once a second while the emulator runs: no frames may mean a freeze */
static void watch_freeze(int64_t now, int frames, bool loading)
{
    if (frames > 0) {
        if (freeze_noticed) {
            xemu_diag_event("the game shows pictures again after %d s",
                            no_frame_seconds);
        }
        if (no_frame_seconds) {
            frame_seconds = 0;
        }
        frame_seconds++;
        no_frame_seconds = 0;
        io_idle_seconds = 0;
        cpu_sample_count = 0;
        freeze_noticed = false;
        return;
    }
    if (frame_seconds < FREEZE_ARM_SECONDS) {
        frame_seconds = 0;
        return;
    }
    no_frame_seconds++;
    io_idle_seconds = loading ? 0 : io_idle_seconds + 1;
    if (cpu_sample_count < CPU_SAMPLES) {
        cpu_samples[cpu_sample_count++] = sample_cpu(no_frame_seconds);
    }

    if (freeze_noticed || no_frame_seconds < FREEZE_SECONDS ||
        (io_idle_seconds < FREEZE_IDLE_IO_SECONDS &&
         no_frame_seconds < FREEZE_ANYWAY_SECONDS)) {
        return;
    }
    freeze_noticed = true;
    if (!g_config.perf.freeze_report) {
        xemu_diag_event("the game stopped showing pictures (%d s)",
                        no_frame_seconds);
        return;
    }
    char *name = write_freeze_report(now);
    if (name) {
        fprintf(stderr, "freeze: no new picture for %d s, report saved to "
                LOGS_DIR "/%s\n", no_frame_seconds, name);
        xemu_diag_event("the game stopped showing pictures (%d s), report "
                        "saved: %s", no_frame_seconds, name);
        qatomic_set(&freeze_us, now);
    }
    g_free(name);
}

static void tick(void *opaque)
{
    if (!xemu_diag_recording()) {
        qatomic_set(&tick_running, 0);
        return;
    }
    timer_mod(tick_timer, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + 1000);

    int64_t now = now_us();
    bool running = runstate_is_running();
    GString *system = g_string_new(NULL);
    bool loading = print_io(system) >= FREEZE_LOADING_BYTES;
    print_system(system);
    if (xemu_diag_active()) {
        static int programs_seconds;
        xemu_diag_threads_sample(system);
        if (programs_seconds++ % PROGRAMS_SECONDS == 0) {
            xemu_diag_threads_scan();
        }
    }

    g_mutex_lock(&lock);
    typeof(sec) last = sec;
    memset(&sec, 0, sizeof(sec));
    int64_t since_frame = frames_seen ? now - last_frame_us : 0;
    if (log_file && running) {
        second_count++;
        if (last.frames < LOW_FPS) {
            low_second_count++;
        }
    }
    g_mutex_unlock(&lock);

    /* Paused: nothing happens that is worth a line */
    if (running) {
        GString *s = g_string_new(NULL);
        g_string_append_printf(s, "frames=%d slow=%d max_gap_ms=%d%s",
                               last.frames, last.slow,
                               (int)(MAX(last.max_gap, since_frame) / 1000),
                               system->str);
        print_work(s, last.count, last.ns, last.max_ns);
        print_gpu(s, last.gpu);
        add_line(now, NULL, s->str);
        g_string_free(s, true);
        watch_freeze(now, last.frames, loading);
    }
    g_string_free(system, true);
}

static void tick_start_bh(void *opaque)
{
    if (!tick_timer) {
        tick_timer = timer_new_ms(QEMU_CLOCK_REALTIME, tick, NULL);
    }
    /* Count disc reads and CPU use from here */
    GString *s = g_string_new(NULL);
    print_io(s);
    print_system(s);
    g_string_free(s, true);
    timer_mod(tick_timer, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + 1000);
}
