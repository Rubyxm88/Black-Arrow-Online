//
// xemu User Interface: Troubleshoot > Make a Full Report on a Mac and on Linux
//
// Runs the Black Arrow Report that setup.sh installs (Linux: report.sh next
// to app/ and system/; Mac: "Black Arrow Report.command" next to the app in
// Applications), as the applications menu or a double-click would, with
// BA_REPORT_FROM_APP set so it opens no window of its own. It puts the logs,
// settings and what the app is doing in one file in Downloads; its "Done:"
// line is shown as a notice. Windows has no report tool.
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#include "full-report.hh"
#include "../xemu-settings.h"
#include "ui/xemu-notifications.h"

#include <SDL3/SDL.h>
#include <glib.h>
#include <string.h>
#include <sys/wait.h>
#include <string>

static GMutex lock;
static bool running;
static bool finished;    // done, its notice not shown yet
static std::string result;

// The installed report tool, or "" when setup.sh did not install one
static std::string ToolPath()
{
    std::string tool;
#if defined(__APPLE__)
    // .../Black Arrow Online.app/Contents/Resources/
    const char *base = SDL_GetBasePath();
    const char *end = base ? strstr(base, ".app/") : NULL;
    if (end) {
        std::string bundle(base, end + 4 - base);
        gchar *dir = g_path_get_dirname(bundle.c_str());
        gchar *path = g_build_filename(dir, "Black Arrow Report.command", NULL);
        tool = path;
        g_free(path);
        g_free(dir);
    }
#else
    // report.sh is next to system/
    gchar *system = g_strdup(xemu_settings_get_base_path());
    size_t n = strlen(system);
    while (n > 1 && system[n - 1] == '/') {
        system[--n] = 0;
    }
    gchar *data = g_path_get_dirname(system);
    gchar *path = g_build_filename(data, "report.sh", NULL);
    tool = path;
    g_free(path);
    g_free(data);
    g_free(system);
#endif
    if (!tool.empty() && !g_file_test(tool.c_str(), G_FILE_TEST_IS_REGULAR)) {
        tool.clear();
    }
    return tool;
}

// What to show: the report's "Done: ..." line, or why there is no file
static std::string Summary(const char *out, bool ok)
{
    std::string line;
    const char *p = out;
    while (p && *p) {
        const char *end = strchr(p, '\n');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if (strncmp(p, "Done: ", 6) == 0 ||
            strncmp(p, "The report could not", 21) == 0) {
            line.assign(p, len);
        }
        p = end ? end + 1 : NULL;
    }
    if (ok && !line.empty()) {
        return line;
    }
    if (line.empty()) {
        line = "The report could not be made.";
    }
#if !defined(__APPLE__)
    line += " What it did is in system/report-run.log.";
#endif
    return line;
}

static gpointer ReportThread(gpointer data)
{
    std::string tool = (const char *)data;
    g_free(data);

    const gchar *argv[] = { "/bin/bash", tool.c_str(), NULL };
    gchar **env = g_environ_setenv(g_get_environ(), "BA_REPORT_FROM_APP", "1",
                                   TRUE);
    gchar *out = NULL;
    gint status = 0;
    GError *err = NULL;
    bool ok = g_spawn_sync(NULL, (gchar **)argv, env, G_SPAWN_DEFAULT, NULL,
                           NULL, &out, NULL, &status, &err) &&
              WIFEXITED(status) && WEXITSTATUS(status) == 0;
    std::string text = Summary(out, ok);
    fprintf(stderr, "report: %s%s%s\n", text.c_str(), err ? " " : "",
            err ? err->message : "");
    g_clear_error(&err);
    g_free(out);
    g_strfreev(env);

    g_mutex_lock(&lock);
    result = text;
    finished = true;
    running = false;
    g_mutex_unlock(&lock);
    return NULL;
}

void FullReportStart()
{
    std::string tool = ToolPath();
    if (tool.empty()) {
        xemu_queue_notification("Black Arrow Report is not installed: run "
                                "setup.sh again to add it.");
        return;
    }
    g_mutex_lock(&lock);
    if (running) {
        g_mutex_unlock(&lock);
        return;
    }
    running = true;
    g_mutex_unlock(&lock);
    xemu_queue_notification("Making a report. This takes about 15 seconds...");
    g_thread_unref(g_thread_new("full-report", ReportThread,
                                g_strdup(tool.c_str())));
}

bool FullReportRunning()
{
    g_mutex_lock(&lock);
    bool r = running;
    g_mutex_unlock(&lock);
    return r;
}

void FullReportPoll()
{
    g_mutex_lock(&lock);
    if (!finished) {
        g_mutex_unlock(&lock);
        return;
    }
    finished = false;
    std::string text = result;
    g_mutex_unlock(&lock);
    xemu_queue_notification(text.c_str());
}
