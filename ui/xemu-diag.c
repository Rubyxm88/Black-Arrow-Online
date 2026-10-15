/*
 * xemu diagnostics log
 *
 * See xemu-diag.h. The frame lines are written by the NV2A profiler
 * (hw/xbox/nv2a/pgraph/profile.c) on the render thread; everything else
 * comes from the UI thread or the connection check thread, so all writes
 * go through one lock.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qemu/atomic.h"
#include "qemu/timer.h"
#include "qemu/xemu-perf.h"
#include "qemu/main-loop.h"
#include "block/aio.h"
#include "block/accounting.h"
#include "system/block-backend.h"
#include <SDL3/SDL.h>
#include <glib/gstdio.h>
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif

#include "xemu-diag.h"
#include "xemu-settings.h"
#include "xemu-os-utils.h"
#include "xemu-xbcheck.h"
#include "xemu-version.h"
#include "hw/xbox/nv2a/shader-precache.h"

#define LOGS_DIR "logs"

/* The worst frames listed in the summary */
#define WORST_FRAMES 10

/* A second with fewer frames than this counts as a low second */
#define LOW_FPS 25

typedef struct WorstFrame {
    double t;
    double ms;
    XemuDiagCause cause;
} WorstFrame;

static GMutex lock;
static FILE *log_file;
static bool active;
static int generation;
static int64_t start_us;
static char *log_path;

/* Summary */
static int64_t frame_count;
static int64_t slow_count[XEMU_DIAG_CAUSE__COUNT];
static double slow_ms[XEMU_DIAG_CAUSE__COUNT];
static int second_count, low_second_count, mark_count;
static WorstFrame worst[WORST_FRAMES];
static int worst_count;

/* Watched by xemu_diag_poll */
static bool had_focus;
static char *last_disc;
static bool cache_reported;

static void io_start_bh(void *opaque);

static const char *const cause_names[XEMU_DIAG_CAUSE__COUNT] = {
    [XEMU_DIAG_CAUSE_SHADER] = "new_shader",
    [XEMU_DIAG_CAUSE_COPYBACK] = "screen_copyback",
    [XEMU_DIAG_CAUSE_CODE] = "code_translate",
    [XEMU_DIAG_CAUSE_LOADING] = "loading_screen",
    [XEMU_DIAG_CAUSE_PAUSE] = "no_frames",
    [XEMU_DIAG_CAUSE_OTHER] = "other",
};

static const char *const cause_text[XEMU_DIAG_CAUSE__COUNT] = {
    [XEMU_DIAG_CAUSE_SHADER] = "building a new shader",
    [XEMU_DIAG_CAUSE_COPYBACK] = "copying the screen back from the GPU",
    [XEMU_DIAG_CAUSE_CODE] = "translating game code",
    [XEMU_DIAG_CAUSE_LOADING] = "loading screen",
    [XEMU_DIAG_CAUSE_PAUSE] = "no frames for 2 s or more (paused, menu, "
                              "window moved)",
    [XEMU_DIAG_CAUSE_OTHER] = "other",
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
            "skip_reuse_copyback=%d use_dsp=%d hard_fpu=%d "
            "cache_shaders=%d\n",
            renderer < 3 ? renderers[renderer] : "?",
            g_config.display.quality.surface_scale,
            g_config.display.window.vsync,
            g_config.display.quality.skip_reuse_copyback,
            g_config.audio.use_dsp,
            g_config.perf.hard_fpu, g_config.perf.cache_shaders);
    fprintf(f, "# network: enabled=%d backend=%s dns_redirect=%s\n",
            g_config.net.enable, backend < 3 ? backends[backend] : "?",
            redirect < 4 ? redirects[redirect] : "?");
    const char *disc = g_config.sys.files.dvd_path;
    fprintf(f, "# disc: %s\n", disc && disc[0] ? base_name(disc) : "none");
}

/* With the lock held */
static void write_line(const char *kind, const char *text)
{
    fprintf(log_file, "%s t=%.2f %s\n", kind, elapsed(), text);
    fflush(log_file);
}

static void write_header(FILE *f)
{
    GDateTime *now = g_date_time_new_now_local();
    char *when = g_date_time_format(now, "%Y-%m-%d %H:%M:%S");
    fprintf(f, "# Black Arrow Online diagnostics log, started %s\n", when);
    g_free(when);
    g_date_time_unref(now);

    write_package(f);
    fprintf(f, "# xemu: %s (%.10s, %s)\n", xemu_version, xemu_commit,
            xemu_date);
    write_machine(f);
    write_settings(f);
    fprintf(f,
        "#\n"
        "# Lines:\n"
        "#   t=SECOND  one per second: frames shown, slow frames, longest "
        "gap, disc and hard disk reads (MB/time spent), cpu (xemu, whole "
        "PC), memory, work done (count/total ms), GPU "
        "counters\n"
        "#   SLOW      a frame of 50 ms or more, with its likely cause and "
        "the work done during it\n"
        "#   MARK      the player pressed F9\n"
        "#   EVENT     focus, disc, connection check, shader cache\n"
        "#   SUMMARY   when recording stops\n"
        "#\n");
    fflush(f);
}

static void reset_summary(void)
{
    frame_count = 0;
    memset(slow_count, 0, sizeof(slow_count));
    memset(slow_ms, 0, sizeof(slow_ms));
    second_count = low_second_count = mark_count = 0;
    worst_count = 0;
}

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
    write_header(f);

    g_mutex_lock(&lock);
    log_file = f;
    g_free(log_path);
    log_path = g_strdup(path);
    generation++;
    start_us = now_us();
    reset_summary();
    had_focus = SDL_GetKeyboardFocus() != NULL;
    g_free(last_disc);
    last_disc = g_strdup(g_config.sys.files.dvd_path);
    cache_reported = false;
    qatomic_set(&active, true);
    g_mutex_unlock(&lock);

    qatomic_set(&xemu_perf_enabled, true);
    aio_bh_schedule_oneshot(qemu_get_aio_context(), io_start_bh, NULL);
    xemu_diag_write_check();

    if (!exit_registered) {
        atexit(at_exit);
        exit_registered = true;
    }
    return true;
}

bool xemu_diag_start(void)
{
    char *dir = logs_dir();
    g_mkdir_with_parents(dir, 0755);

    GDateTime *now = g_date_time_new_now_local();
    char *stamp = g_date_time_format(now, "%Y-%m-%d_%H-%M-%S");
    char *name = g_strdup_printf("diagnostics-%s.txt", stamp);
    char *path = g_build_filename(dir, name, NULL);
    bool ok = xemu_diag_start_path(path);

    g_free(path);
    g_free(name);
    g_free(stamp);
    g_date_time_unref(now);
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
    qatomic_set(&xemu_perf_enabled, false);
}

bool xemu_diag_active(void)
{
    return qatomic_read(&active);
}

double xemu_diag_seconds(void)
{
    return xemu_diag_active() ? elapsed() : 0;
}

const char *xemu_diag_path(void)
{
    return log_path;
}

void xemu_diag_open_folder(void)
{
    char *dir = logs_dir();
    char *path = g_canonicalize_filename(dir, NULL);
    g_mkdir_with_parents(path, 0755);
#ifdef _WIN32
    /* Explorer opens a folder given as a plain path */
    char *url = g_strdup(path);
#else
    char *url = g_filename_to_uri(path, NULL, NULL);
#endif
    if (url) {
        SDL_OpenURL(url);
    }
    g_free(url);
    g_free(path);
    g_free(dir);
}

void xemu_diag_mark(void)
{
    g_mutex_lock(&lock);
    if (log_file) {
        char text[32];
        snprintf(text, sizeof(text), "n=%d", ++mark_count);
        write_line("MARK", text);
    }
    g_mutex_unlock(&lock);
}

void xemu_diag_event(const char *fmt, ...)
{
    if (!xemu_diag_active()) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    char *text = g_strdup_vprintf(fmt, ap);
    va_end(ap);

    g_mutex_lock(&lock);
    if (log_file) {
        write_line("EVENT", text);
    }
    g_mutex_unlock(&lock);
    g_free(text);
}

void xemu_diag_write_check(void)
{
    static const char *const states[] = { "ok", "info", "warn", "fail" };
    XbCheckItem items[XBCHECK_MAX_ITEMS];
    bool running;

    if (!xemu_diag_active()) {
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
    if (!xemu_diag_active()) {
        return;
    }

    bool focus = SDL_GetKeyboardFocus() != NULL;
    if (focus != had_focus) {
        had_focus = focus;
        xemu_diag_event("%s", focus ? "window got focus" : "window lost focus");
    }

    const char *disc = g_config.sys.files.dvd_path;
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

FILE *xemu_diag_lock(int *gen, int64_t *start)
{
    if (!xemu_diag_active()) {
        return NULL;
    }
    g_mutex_lock(&lock);
    if (!log_file) {
        g_mutex_unlock(&lock);
        return NULL;
    }
    *gen = generation;
    *start = start_us;
    return log_file;
}

void xemu_diag_unlock(void)
{
    g_mutex_unlock(&lock);
}

void xemu_diag_count_frame(int64_t now, int64_t gap_us, bool slow,
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

void xemu_diag_count_second(int frames)
{
    second_count++;
    if (frames < LOW_FPS) {
        low_second_count++;
    }
}

/*
 * Disc and hard disk reads. The block layer keeps byte and time totals per
 * drive, but may only be walked from the main loop, so a timer there copies
 * them every IO_SAMPLE_MS for the frame logger to print.
 */
#define IO_SAMPLE_MS 250

enum { IO_HDD, IO_DISC, IO__COUNT };
enum { IO_READ_BYTES, IO_READ_NS, IO_WRITE_BYTES, IO__FIELDS };

static uint64_t io_totals[IO__COUNT][IO__FIELDS];
static QEMUTimer *io_timer;

static void io_sample(void *opaque)
{
    uint64_t v[IO__COUNT][IO__FIELDS] = { 0 };

    for (BlockBackend *blk = blk_next(NULL); blk; blk = blk_next(blk)) {
        BlockAcctStats *stats = blk_get_stats(blk);
        int drive = strstr(blk_name(blk), "-cd") ? IO_DISC : IO_HDD;
        v[drive][IO_READ_BYTES] += stats->nr_bytes[BLOCK_ACCT_READ];
        v[drive][IO_READ_NS] += stats->total_time_ns[BLOCK_ACCT_READ];
        v[drive][IO_WRITE_BYTES] += stats->nr_bytes[BLOCK_ACCT_WRITE];
    }
    for (int d = 0; d < IO__COUNT; d++) {
        for (int i = 0; i < IO__FIELDS; i++) {
            qatomic_set(&io_totals[d][i], v[d][i]);
        }
    }
    if (xemu_diag_active()) {
        timer_mod(io_timer,
                  qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + IO_SAMPLE_MS);
    }
}

static void io_start_bh(void *opaque)
{
    if (!io_timer) {
        io_timer = timer_new_ms(QEMU_CLOCK_REALTIME, io_sample, NULL);
    }
    io_sample(NULL);
}

/* " disc_read=MB/ms hdd_read=MB/ms hdd_write=MB" since the last call */
static void print_io(FILE *f)
{
    static const char *const names[IO__COUNT] = { "hdd", "disc" };
    static uint64_t last[IO__COUNT][IO__FIELDS];
    static int last_generation = -1;

    if (last_generation != generation) {
        /* First second of a recording: only take the starting point */
        last_generation = generation;
        for (int d = 0; d < IO__COUNT; d++) {
            for (int i = 0; i < IO__FIELDS; i++) {
                last[d][i] = qatomic_read(&io_totals[d][i]);
            }
        }
        return;
    }
    for (int d = IO__COUNT - 1; d >= 0; d--) {
        uint64_t now[IO__FIELDS];
        for (int i = 0; i < IO__FIELDS; i++) {
            now[i] = qatomic_read(&io_totals[d][i]);
        }
        uint64_t rd = now[IO_READ_BYTES] - last[d][IO_READ_BYTES];
        uint64_t rd_ns = now[IO_READ_NS] - last[d][IO_READ_NS];
        uint64_t wr = now[IO_WRITE_BYTES] - last[d][IO_WRITE_BYTES];
        if (rd) {
            fprintf(f, " %s_read=%.1fMB/%.0fms", names[d], rd / 1048576.0,
                    rd_ns / 1e6);
        }
        if (wr) {
            fprintf(f, " %s_write=%.1fMB", names[d], wr / 1048576.0);
        }
        memcpy(last[d], now, sizeof(now));
    }
}

#ifdef _WIN32
static ULONGLONG filetime(FILETIME ft)
{
    return ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}
#endif

void xemu_diag_print_system(FILE *f)
{
    print_io(f);
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
        fprintf(f, " cpu=%.0f%% sys_cpu=%.0f%%",
                100.0 * (proc - last_proc) / ((wall - last_wall) * cpus),
                100.0 * (busy - last_busy) / (total - last_total));
    }
    last_proc = proc;
    last_busy = busy;
    last_total = total;
    last_wall = wall;

    PROCESS_MEMORY_COUNTERS mem = { .cb = sizeof(mem) };
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), &mem, sizeof(mem))) {
        fprintf(f, " mem_mb=%zu", (size_t)(mem.WorkingSetSize >> 20));
    }
#endif
}
