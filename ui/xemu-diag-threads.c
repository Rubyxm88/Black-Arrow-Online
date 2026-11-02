/*
 * xemu diagnostics log: CPU use per emulator thread and of other programs
 *
 * See xemu-diag-threads.h. Busy time comes from the thread cycle counters
 * against the time stamp counter, which tick at the same rate, so short
 * stretches (one frame) are measured as well as long ones; the plain thread
 * times only move in 15.6 ms steps.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qemu/atomic.h"
#include "xemu-diag-threads.h"
#include "xemu-diag.h"

#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#include <intrin.h>

/* Other programs: list them when together they use this share of the PC */
#define PROGRAMS_NOTABLE_PERCENT 15
#define PROGRAMS_LISTED 3
#define PROGRAMS_SUMMARY 5

enum {
    G_GAME_CPU, G_GPU, G_UI, G_AUDIO, G_VOICES, G_MAIN_LOOP, G_OTHER,
    G__COUNT
};

static const char *const group_names[G__COUNT] = {
    [G_GAME_CPU] = "game_cpu",   [G_GPU] = "gpu",
    [G_UI] = "ui",               [G_AUDIO] = "audio",
    [G_VOICES] = "voices",       [G_MAIN_LOOP] = "main_loop",
    [G_OTHER] = "other",
};

typedef struct ThreadInfo {
    DWORD tid;
    HANDLE handle;
    char *name;
    int group;
    ULONG64 last_cycles;
} ThreadInfo;

static GMutex lock;
static bool programs_reset_pending = true;
static GArray *threads;
static ULONG64 last_tsc;
static DWORD ui_tid;

/*
 * Set while the scan thread runs. Listing every thread and program on the PC
 * takes up to 100 ms, which on the main loop held up the game every scan.
 */
static bool scan_busy;

/* For the render thread's per-frame reading; opened once, never closed */
static HANDLE game_cpu_handle;

/* Summary: per group, the sum and peak of the per-second percentages */
static double group_sum[G__COUNT], group_peak[G__COUNT];
static int sample_count;

typedef HRESULT (WINAPI *GetThreadDescriptionFn)(HANDLE, PWSTR *);

static char *thread_name(HANDLE h)
{
    static GetThreadDescriptionFn get_description;
    static gsize init;
    if (g_once_init_enter(&init)) {
        HMODULE k32 = GetModuleHandleA("kernel32.dll");
        get_description = k32 ? (GetThreadDescriptionFn)(void (*)(void))
                                GetProcAddress(k32, "GetThreadDescription")
                              : NULL;
        g_once_init_leave(&init, 1);
    }

    PWSTR desc = NULL;
    char *name = NULL;
    if (get_description && SUCCEEDED(get_description(h, &desc)) && desc) {
        name = g_utf16_to_utf8((const gunichar2 *)desc, -1, NULL, NULL, NULL);
        LocalFree(desc);
    }
    if (name) {
        /* One word, so the line stays easy to split */
        g_strdelimit(name, " ,:=", '_');
    }
    return name;
}

static int classify(DWORD tid, const char *name)
{
    if (tid == qatomic_read(&ui_tid)) {
        return G_UI;
    }
    if (!name || !name[0]) {
        return G_OTHER;
    }
    if (g_str_has_prefix(name, "CPU_") || g_str_has_prefix(name, "ALL_CPUs")) {
        return G_GAME_CPU;
    }
    if (!strcmp(name, "nv2a.pfifo_thread")) {
        return G_GPU;
    }
    if (!strcmp(name, "mcpx.apu_thread")) {
        return G_AUDIO;
    }
    if (!strcmp(name, "mcpx.voice_worker")) {
        return G_VOICES;
    }
    if (!strcmp(name, "qemu_main")) {
        return G_MAIN_LOOP;
    }
    return G_OTHER;
}

static ThreadInfo *find_thread(DWORD tid)
{
    for (guint i = 0; i < threads->len; i++) {
        ThreadInfo *t = &g_array_index(threads, ThreadInfo, i);
        if (t->tid == tid) {
            return t;
        }
    }
    return NULL;
}

/* Scan thread: add the threads of this process not known yet */
static void scan_threads(void)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return;
    }
    GArray *tids = g_array_new(false, false, sizeof(DWORD));
    DWORD pid = GetCurrentProcessId();
    THREADENTRY32 te = { .dwSize = sizeof(te) };
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID == pid) {
            g_array_append_val(tids, te.th32ThreadID);
        }
    }
    CloseHandle(snap);

    g_mutex_lock(&lock);
    if (!threads) {
        threads = g_array_new(false, false, sizeof(ThreadInfo));
    }
    for (guint i = 0; i < tids->len; i++) {
        DWORD tid = g_array_index(tids, DWORD, i);
        ThreadInfo *known = find_thread(tid);
        if (known) {
            /* A thread seen before it was named */
            if (!known->name) {
                known->name = thread_name(known->handle);
                known->group = classify(known->tid, known->name);
            }
            continue;
        }
        HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
        if (!h) {
            continue;
        }
        ThreadInfo t = { .tid = tid, .handle = h };
        t.name = thread_name(h);
        t.group = classify(t.tid, t.name);
        QueryThreadCycleTime(h, &t.last_cycles);
        g_array_append_val(threads, t);

        if (t.group == G_GAME_CPU && !qatomic_read(&game_cpu_handle)) {
            HANDLE own = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE,
                                    t.tid);
            qatomic_set(&game_cpu_handle, own);
        }
    }
    g_mutex_unlock(&lock);
    g_array_free(tids, true);
}

void xemu_diag_threads_reset(void)
{
    g_mutex_lock(&lock);
    memset(group_sum, 0, sizeof(group_sum));
    memset(group_peak, 0, sizeof(group_peak));
    sample_count = 0;
    programs_reset_pending = true;
    g_mutex_unlock(&lock);
}

void xemu_diag_threads_mark_ui(void)
{
    qatomic_set(&ui_tid, GetCurrentThreadId());
}

void xemu_diag_threads_sample(GString *s)
{
    g_mutex_lock(&lock);
    if (!threads) {
        /* The first scan has not finished */
        g_mutex_unlock(&lock);
        return;
    }

    ULONG64 tsc = __rdtsc();
    ULONG64 span = tsc - last_tsc;
    bool have_span = last_tsc && tsc > last_tsc;
    last_tsc = tsc;

    double group[G__COUNT] = { 0 };
    double top_other = 0;
    const char *top_name = NULL;
    for (guint i = 0; i < threads->len;) {
        ThreadInfo *t = &g_array_index(threads, ThreadInfo, i);
        ULONG64 cycles = t->last_cycles;
        QueryThreadCycleTime(t->handle, &cycles);
        double pct = have_span ? 100.0 * (cycles - t->last_cycles) / span : 0;
        t->last_cycles = cycles;
        group[t->group] += pct;
        if (t->group == G_OTHER && pct > top_other) {
            top_other = pct;
            top_name = t->name;
        }

        DWORD code;
        if (GetExitCodeThread(t->handle, &code) && code != STILL_ACTIVE) {
            if (top_name == t->name) {
                top_name = "exited";
            }
            CloseHandle(t->handle);
            g_free(t->name);
            g_array_remove_index_fast(threads, i);
        } else {
            i++;
        }
    }

    if (have_span) {
        g_string_append(s, " thread_cpu=");
        for (int g = 0; g < G__COUNT; g++) {
            g_string_append_printf(s, "%s%s:%.0f", g ? "," : "",
                                   group_names[g], group[g]);
            group_sum[g] += group[g];
            group_peak[g] = MAX(group_peak[g], group[g]);
        }
        sample_count++;
        if (top_other >= 1) {
            g_string_append_printf(s, " busiest_other=%s:%.0f",
                                   top_name && top_name[0] ? top_name : "unnamed", top_other);
        }
    }
    g_mutex_unlock(&lock);
}

void xemu_diag_threads_summary(GString *s)
{
    g_mutex_lock(&lock);
    if (sample_count) {
        g_string_append(s, "SUMMARY threads, average (peak) % of one core:");
        for (int g = 0; g < G__COUNT; g++) {
            g_string_append_printf(s, " %s %.0f (%.0f)", group_names[g],
                                   group_sum[g] / sample_count,
                                   group_peak[g]);
        }
        g_string_append_c(s, '\n');
    }
    g_mutex_unlock(&lock);
}

double xemu_diag_threads_game_cpu(void)
{
    static ULONG64 last_cycles, last_frame_tsc;
    HANDLE h = qatomic_read(&game_cpu_handle);
    ULONG64 cycles;

    if (!h || !QueryThreadCycleTime(h, &cycles)) {
        return -1;
    }
    ULONG64 tsc = __rdtsc();
    double busy = -1;
    if (last_frame_tsc && tsc > last_frame_tsc && cycles >= last_cycles) {
        busy = (double)(cycles - last_cycles) / (tsc - last_frame_tsc);
    }
    last_cycles = cycles;
    last_frame_tsc = tsc;
    return busy;
}

/*
 * Other programs: CPU time per process ID (scan thread only), and totals per
 * program name since the reset (with the lock held)
 */
static GHashTable *process_last;   /* pid -> ULONG64 time (100 ns) */
static ULONG64 programs_last_wall;
static GHashTable *program_total;  /* name -> ULONG64 time since reset */
static ULONG64 programs_wall_total;

static ULONG64 filetime64(FILETIME ft)
{
    return ((ULONG64)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

static ULONG64 wall_now(void)
{
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return filetime64(ft);
}

typedef struct ProgramShare {
    const char *name;
    ULONG64 time;
} ProgramShare;

static gint by_time(gconstpointer a, gconstpointer b)
{
    ULONG64 ta = ((const ProgramShare *)a)->time;
    ULONG64 tb = ((const ProgramShare *)b)->time;
    return ta < tb ? 1 : ta > tb ? -1 : 0;
}

/* Sorted shares, busiest first, from a name -> time table */
static GArray *sorted_shares(GHashTable *table)
{
    GArray *a = g_array_new(false, false, sizeof(ProgramShare));
    GHashTableIter it;
    gpointer key, value;
    g_hash_table_iter_init(&it, table);
    while (g_hash_table_iter_next(&it, &key, &value)) {
        ProgramShare p = { key, *(ULONG64 *)value };
        g_array_append_val(a, p);
    }
    g_array_sort(a, by_time);
    return a;
}

static void add_time(GHashTable *table, const char *name, ULONG64 time)
{
    ULONG64 *v = g_hash_table_lookup(table, name);
    if (!v) {
        v = g_new0(ULONG64, 1);
        g_hash_table_insert(table, g_strdup(name), v);
    }
    *v += time;
}


/*
 * Scan thread: other programs using the PC. Returns "name N%, ..." (percent
 * of the whole PC) when they used a fair share since the last call, else
 * NULL.
 */
static char *programs_sample(void)
{
    if (!process_last) {
        process_last = g_hash_table_new_full(NULL, NULL, NULL, g_free);
    }

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return NULL;
    }
    ULONG64 wall = wall_now();
    ULONG64 span = programs_last_wall && wall > programs_last_wall ?
                   wall - programs_last_wall : 0;
    programs_last_wall = wall;

    GHashTable *now_by_name = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                    g_free, g_free);
    GHashTable *seen = g_hash_table_new_full(NULL, NULL, NULL, g_free);
    DWORD self = GetCurrentProcessId();
    PROCESSENTRY32W pe = { .dwSize = sizeof(pe) };
    for (BOOL ok = Process32FirstW(snap, &pe); ok;
         ok = Process32NextW(snap, &pe)) {
        if (pe.th32ProcessID == self || pe.th32ProcessID == 0) {
            continue;
        }
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                               pe.th32ProcessID);
        if (!h) {
            continue;
        }
        FILETIME c, e, k, u;
        if (GetProcessTimes(h, &c, &e, &k, &u)) {
            ULONG64 *time = g_new(ULONG64, 1);
            *time = filetime64(k) + filetime64(u);
            gpointer key = GUINT_TO_POINTER(pe.th32ProcessID);
            ULONG64 *last = g_hash_table_lookup(process_last, key);
            if (last && span && *time >= *last) {
                char *name = g_utf16_to_utf8((const gunichar2 *)pe.szExeFile,
                                             -1, NULL, NULL, NULL);
                if (name) {
                    g_strdelimit(name, " ,:=", '_');
                    add_time(now_by_name, name, *time - *last);
                    g_free(name);
                }
            }
            g_hash_table_insert(seen, key, time);
        }
        CloseHandle(h);
    }
    CloseHandle(snap);
    g_hash_table_unref(process_last);
    process_last = seen;

    g_mutex_lock(&lock);
    if (!program_total) {
        program_total = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                              g_free);
    }
    if (programs_reset_pending) {
        programs_reset_pending = false;
        g_hash_table_remove_all(program_total);
        programs_wall_total = 0;
    }
    if (span) {
        GHashTableIter it;
        gpointer key, value;
        g_hash_table_iter_init(&it, now_by_name);
        while (g_hash_table_iter_next(&it, &key, &value)) {
            add_time(program_total, key, *(ULONG64 *)value);
        }
        programs_wall_total += span;
    }
    g_mutex_unlock(&lock);

    char *text = NULL;
    if (span) {
        double cores = g_get_num_processors();
        GArray *a = sorted_shares(now_by_name);
        double sum = 0;
        for (guint i = 0; i < a->len; i++) {
            sum += 100.0 * g_array_index(a, ProgramShare, i).time /
                   (span * cores);
        }
        if (sum >= PROGRAMS_NOTABLE_PERCENT) {
            GString *s = g_string_new(NULL);
            g_string_append_printf(s, "%.0f%% in total:", sum);
            for (guint i = 0; i < a->len && i < PROGRAMS_LISTED; i++) {
                ProgramShare *p = &g_array_index(a, ProgramShare, i);
                g_string_append_printf(s, "%s %s %.0f%%", i ? "," : "",
                                       p->name,
                                       100.0 * p->time / (span * cores));
            }
            text = g_string_free(s, false);
        }
        g_array_free(a, true);
    }
    g_hash_table_unref(now_by_name);
    return text;
}

static gpointer scan_worker(gpointer opaque)
{
    scan_threads();
    char *programs = programs_sample();
    if (programs) {
        xemu_diag_event("other programs using the PC, %s", programs);
        g_free(programs);
    }
    qatomic_set(&scan_busy, false);
    return NULL;
}

void xemu_diag_threads_scan(void)
{
    if (!qatomic_xchg(&scan_busy, true)) {
        g_thread_unref(g_thread_new("diag.scan", scan_worker, NULL));
    }
}

void xemu_diag_programs_summary(GString *s)
{
    g_mutex_lock(&lock);
    if (program_total && programs_wall_total) {
        double cores = g_get_num_processors();
        GArray *a = sorted_shares(program_total);
        double sum = 0;
        for (guint i = 0; i < a->len; i++) {
            sum += 100.0 * g_array_index(a, ProgramShare, i).time /
                   (programs_wall_total * cores);
        }
        g_string_append_printf(s, "SUMMARY other programs, average %% of the "
                               "whole PC: %.0f in total", sum);
        for (guint i = 0; i < a->len && i < PROGRAMS_SUMMARY; i++) {
            ProgramShare *p = &g_array_index(a, ProgramShare, i);
            g_string_append_printf(s, ", %s %.1f", p->name,
                                   100.0 * p->time /
                                   (programs_wall_total * cores));
        }
        g_string_append_c(s, '\n');
        g_array_free(a, true);
    }
    g_mutex_unlock(&lock);
}

#elif defined(__linux__)

/*
 * Linux: the threads are the entries of /proc/self/task, named by their comm
 * (15 characters at most, so "nv2a.pfifo_thre"). Busy time comes from each
 * thread's CPU clock, in nanoseconds. Other programs are not sampled.
 */
#include <dirent.h>
#include <sys/syscall.h>
#include <time.h>

enum {
    G_GAME_CPU, G_GPU, G_UI, G_AUDIO, G_VOICES, G_MAIN_LOOP, G_OTHER,
    G__COUNT
};

static const char *const group_names[G__COUNT] = {
    [G_GAME_CPU] = "game_cpu",   [G_GPU] = "gpu",
    [G_UI] = "ui",               [G_AUDIO] = "audio",
    [G_VOICES] = "voices",       [G_MAIN_LOOP] = "main_loop",
    [G_OTHER] = "other",
};

typedef struct ThreadInfo {
    pid_t tid;
    char *name;
    int group;
    int64_t last_ns;
} ThreadInfo;

static GMutex lock;
static GArray *threads;
static int64_t last_wall_ns;
static pid_t ui_tid;
static pid_t game_cpu_tid;
static bool scan_busy;

static double group_sum[G__COUNT], group_peak[G__COUNT];
static int sample_count;

static int64_t wall_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/* Another thread's CPU time: its CPU clock, as glibc makes it for a tid */
static bool thread_ns(pid_t tid, int64_t *ns)
{
    clockid_t clock = (clockid_t)((~(unsigned)tid << 3) | 4 | 2);
    struct timespec ts;
    if (clock_gettime(clock, &ts)) {
        return false;
    }
    *ns = ts.tv_sec * 1000000000LL + ts.tv_nsec;
    return true;
}

static char *thread_name(pid_t tid)
{
    char *path = g_strdup_printf("/proc/self/task/%d/comm", (int)tid);
    char *name = NULL;
    if (g_file_get_contents(path, &name, NULL, NULL)) {
        g_strstrip(name);
        /* One word, so the line stays easy to split */
        g_strdelimit(name, " ,:=/", '_');
    }
    g_free(path);
    return name;
}

static int classify(pid_t tid, const char *name)
{
    if (tid == qatomic_read(&ui_tid)) {
        return G_UI;
    }
    if (!name || !name[0]) {
        return G_OTHER;
    }
    if (g_str_has_prefix(name, "CPU_") || g_str_has_prefix(name, "ALL_CPUs")) {
        return G_GAME_CPU;
    }
    if (g_str_has_prefix(name, "nv2a.pfifo")) {
        return G_GPU;
    }
    if (!strcmp(name, "mcpx.apu_thread")) {
        return G_AUDIO;
    }
    if (g_str_has_prefix(name, "mcpx.voice")) {
        return G_VOICES;
    }
    if (!strcmp(name, "qemu_main")) {
        return G_MAIN_LOOP;
    }
    return G_OTHER;
}

static ThreadInfo *find_thread(pid_t tid)
{
    for (guint i = 0; i < threads->len; i++) {
        ThreadInfo *t = &g_array_index(threads, ThreadInfo, i);
        if (t->tid == tid) {
            return t;
        }
    }
    return NULL;
}

/* Scan thread: add the threads of this process not known yet */
static void scan_threads(void)
{
    DIR *dir = opendir("/proc/self/task");
    if (!dir) {
        return;
    }
    GArray *tids = g_array_new(false, false, sizeof(pid_t));
    struct dirent *e;
    while ((e = readdir(dir))) {
        pid_t tid = atoi(e->d_name);
        if (tid > 0) {
            g_array_append_val(tids, tid);
        }
    }
    closedir(dir);

    g_mutex_lock(&lock);
    if (!threads) {
        threads = g_array_new(false, false, sizeof(ThreadInfo));
    }
    for (guint i = 0; i < tids->len; i++) {
        pid_t tid = g_array_index(tids, pid_t, i);
        ThreadInfo *known = find_thread(tid);
        if (known) {
            /* Named after it started, or the window thread marked since */
            g_free(known->name);
            known->name = thread_name(tid);
            known->group = classify(tid, known->name);
            continue;
        }
        ThreadInfo t = { .tid = tid };
        if (!thread_ns(tid, &t.last_ns)) {
            continue;
        }
        t.name = thread_name(tid);
        t.group = classify(tid, t.name);
        g_array_append_val(threads, t);
        if (t.group == G_GAME_CPU && !qatomic_read(&game_cpu_tid)) {
            qatomic_set(&game_cpu_tid, tid);
        }
    }
    g_mutex_unlock(&lock);
    g_array_free(tids, true);
}

void xemu_diag_threads_reset(void)
{
    g_mutex_lock(&lock);
    memset(group_sum, 0, sizeof(group_sum));
    memset(group_peak, 0, sizeof(group_peak));
    sample_count = 0;
    g_mutex_unlock(&lock);
}

void xemu_diag_threads_mark_ui(void)
{
    qatomic_set(&ui_tid, (pid_t)syscall(SYS_gettid));
}

void xemu_diag_threads_sample(GString *s)
{
    g_mutex_lock(&lock);
    if (!threads) {
        /* The first scan has not finished */
        g_mutex_unlock(&lock);
        return;
    }

    int64_t now = wall_ns();
    int64_t span = now - last_wall_ns;
    bool have_span = last_wall_ns && span > 0;
    last_wall_ns = now;

    double group[G__COUNT] = { 0 };
    double top_other = 0;
    const char *top_name = NULL;
    for (guint i = 0; i < threads->len;) {
        ThreadInfo *t = &g_array_index(threads, ThreadInfo, i);
        int64_t ns;
        if (!thread_ns(t->tid, &ns)) {
            /* Exited */
            g_free(t->name);
            g_array_remove_index_fast(threads, i);
            continue;
        }
        double pct = have_span ? 100.0 * (ns - t->last_ns) / span : 0;
        t->last_ns = ns;
        group[t->group] += pct;
        if (t->group == G_OTHER && pct > top_other) {
            top_other = pct;
            top_name = t->name;
        }
        i++;
    }

    if (have_span) {
        g_string_append(s, " thread_cpu=");
        for (int g = 0; g < G__COUNT; g++) {
            g_string_append_printf(s, "%s%s:%.0f", g ? "," : "",
                                   group_names[g], group[g]);
            group_sum[g] += group[g];
            group_peak[g] = MAX(group_peak[g], group[g]);
        }
        sample_count++;
        if (top_other >= 1) {
            g_string_append_printf(s, " busiest_other=%s:%.0f",
                                   top_name && top_name[0] ? top_name : "unnamed", top_other);
        }
    }
    g_mutex_unlock(&lock);
}

void xemu_diag_threads_summary(GString *s)
{
    g_mutex_lock(&lock);
    if (sample_count) {
        g_string_append(s, "SUMMARY threads, average (peak) % of one core:");
        for (int g = 0; g < G__COUNT; g++) {
            g_string_append_printf(s, " %s %.0f (%.0f)", group_names[g],
                                   group_sum[g] / sample_count,
                                   group_peak[g]);
        }
        g_string_append_c(s, '\n');
    }
    g_mutex_unlock(&lock);
}

double xemu_diag_threads_game_cpu(void)
{
    static int64_t last_ns, last_frame_ns;
    pid_t tid = qatomic_read(&game_cpu_tid);
    int64_t ns;

    if (!tid || !thread_ns(tid, &ns)) {
        return -1;
    }
    int64_t now = wall_ns();
    double busy = -1;
    if (last_frame_ns && now > last_frame_ns && ns >= last_ns) {
        busy = (double)(ns - last_ns) / (now - last_frame_ns);
    }
    last_ns = ns;
    last_frame_ns = now;
    return busy;
}

static gpointer scan_worker(gpointer opaque)
{
    scan_threads();
    qatomic_set(&scan_busy, false);
    return NULL;
}

void xemu_diag_threads_scan(void)
{
    if (!qatomic_xchg(&scan_busy, true)) {
        g_thread_unref(g_thread_new("diag.scan", scan_worker, NULL));
    }
}

void xemu_diag_programs_summary(GString *s) {}

#else

void xemu_diag_threads_reset(void) {}
void xemu_diag_threads_mark_ui(void) {}
void xemu_diag_threads_sample(GString *s) {}
void xemu_diag_threads_summary(GString *s) {}
double xemu_diag_threads_game_cpu(void) { return -1; }
void xemu_diag_threads_scan(void) {}
void xemu_diag_programs_summary(GString *s) {}

#endif
