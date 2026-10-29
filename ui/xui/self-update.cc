//
// xemu User Interface: Black Arrow Online updates on a Mac and on Linux
//
// A few seconds after the start the app reads mac-version.txt (Linux:
// linux-version.txt) of the newest GitHub release. When it names a newer
// build than the one installed (the version.txt setup.sh put in the console
// folder), a small panel offers it. Update now downloads the build's update
// (BlackArrowOnline-mac-update.zip, or BlackArrowOnline-linux-update.tar.gz),
// checks it against the SHA-256 in that file and unpacks it; then the app
// starts the update's own setup.sh with --update and closes. setup.sh waits
// for it to close, installs the new app as a Terminal run would, and opens
// it again. Downloaded here and not in a browser, nothing is marked as
// downloaded, so macOS has nothing to approve.
//
// setup.sh leaves update-done.txt (the new build's number) or
// update-failed.txt (why it stopped) in the console folder; the next start
// shows it once.
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#include "common.hh"
#include "self-update.hh"
#include "versions.hh"
#include "actions.hh"
#include "notifications.hh"
#include "viewport-manager.hh"
#include "../xemu-settings.h"
#include "ui/xemu-notifications.h"

extern "C" {
#include "qapi/error.h"
}

#include <SDL3/SDL.h>
#include <glib/gstdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <limits.h>

#define RELEASES_PAGE "https://github.com/Rubyxm88/Black-Arrow-Online/releases"

#if defined(__APPLE__)
#define PLATFORM "Mac"
#define INFO_NAME "mac-version.txt"
#define ZIP_NAME "BlackArrowOnline-mac-update.zip"
#else
#define PLATFORM "Linux"
#define INFO_NAME "linux-version.txt"
#define ZIP_NAME "BlackArrowOnline-linux-update.tar.gz"
#endif

// The releases page; XEMU_TEST_RELEASES points it at a test server that has
// the same download/... layout
static std::string Releases()
{
    const char *test = g_getenv("XEMU_TEST_RELEASES");
    return test && *test ? test : RELEASES_PAGE;
}

/* Seconds after starting before looking for an update */
#define AUTO_DELAY 10.0

static const ImVec4 xbox_green(0.063f, 0.486f, 0.063f, 1.0f);
static const ImVec4 xbox_green_hover(0.10f, 0.62f, 0.10f, 1.0f);
static const ImVec4 xbox_green_active(0.05f, 0.40f, 0.05f, 1.0f);
static const ImVec4 problem_color(1.0f, 0.55f, 0.35f, 1.0f);

SelfUpdateWindow self_update_window;

// A key=value line of version.txt or mac-version.txt
static std::string Field(const std::string &text, const char *key)
{
    std::string prefix = std::string(key) + "=";
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) {
            end = text.size();
        }
        if (text.compare(pos, prefix.size(), prefix) == 0) {
            std::string value =
                text.substr(pos + prefix.size(), end - pos - prefix.size());
            while (!value.empty() &&
                   (value.back() == '\r' || value.back() == ' ')) {
                value.pop_back();
            }
            return value;
        }
        pos = end + 1;
    }
    return "";
}

// A file in the console folder (system/)
static std::string ConsoleFile(const char *name)
{
    return std::string(xemu_settings_get_base_path()) + name;
}

static std::string ReadText(const std::string &path)
{
    gchar *text = NULL;
    std::string out;
    if (g_file_get_contents(path.c_str(), &text, NULL, NULL)) {
        out = text;
        g_free(text);
    }
    return out;
}

// The installed build; 0 for a beta or test build, which any build replaces
static int LocalBuild()
{
    return atoi(Field(ReadText(ConsoleFile("version.txt")), "build").c_str());
}

static void PushGreenButtons()
{
    ImGui::PushStyleColor(ImGuiCol_Button, xbox_green);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, xbox_green_hover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, xbox_green_active);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
}

static void PopButtonColors()
{
    ImGui::PopStyleColor(4);
}

SelfUpdateWindow::SelfUpdateWindow()
{
    g_mutex_init(&m_lock);
    is_open = false;
    m_state = IDLE;
    m_local_build = 0;
    m_build = 0;
    m_dlnow = 0;
    m_dltotal = 0;
    m_cancel = false;
    m_started = false;
    m_auto_checked = false;
    m_quiet = true;
    m_later = false;
    m_restarting = false;
    m_wrong_copy = false;
}

#if defined(__APPLE__)
// Opened somewhere else than where setup.sh installs the app, usually from
// the unzipped download (macOS then runs it from a hidden copy in
// AppTranslocation). Updates replace the copy in Applications, so this one
// would never change.
static bool WrongCopy()
{
    const char *base = SDL_GetBasePath();
    if (!base) {
        return false;
    }
    std::string path = base;
    std::string home_apps = std::string(g_get_home_dir()) + "/Applications/";
    bool translocated = path.find("/AppTranslocation/") != std::string::npos;
    bool in_apps = path.rfind("/Applications/", 0) == 0 ||
                   path.rfind(home_apps, 0) == 0;
    bool installed =
        g_file_test("/Applications/Black Arrow Online.app",
                    G_FILE_TEST_IS_DIR) ||
        g_file_test((home_apps + "Black Arrow Online.app").c_str(),
                    G_FILE_TEST_IS_DIR);
    if (translocated || (!in_apps && installed)) {
        fprintf(stderr, "update: running from %s, not the copy in "
                "Applications\n", base);
        return true;
    }
    return false;
}
#else
// The app folder setup.sh installs, next to the console folder (system/)
static std::string AppDir()
{
    gchar *system = g_strdup(xemu_settings_get_base_path());
    size_t n = strlen(system);
    while (n > 1 && system[n - 1] == '/') {
        system[--n] = 0;
    }
    gchar *data = g_path_get_dirname(system);
    gchar *app = g_build_filename(data, "app", NULL);
    std::string out = app;
    g_free(app);
    g_free(data);
    g_free(system);
    return out;
}

// Opened somewhere else than the copy setup.sh installed, usually from the
// unpacked download. Updates replace the installed copy, so this one would
// never change.
static bool WrongCopy()
{
    const char *base = SDL_GetBasePath();
    std::string app = AppDir();
    char real[PATH_MAX];
    if (!base || !g_file_test((app + "/AppRun").c_str(), G_FILE_TEST_EXISTS) ||
        !realpath(app.c_str(), real)) {
        return false;
    }
    std::string prefix = std::string(real) + "/";
    if (std::string(base).rfind(prefix, 0) != 0) {
        fprintf(stderr, "update: running from %s, not the installed copy in "
                "%s\n", base, real);
        return true;
    }
    return false;
}
#endif

void SelfUpdateWindow::DrawWrongCopy()
{
    float scale = g_viewport_mgr.m_scale;
    PopupStackPlace();
    ImGui::SetNextWindowBgAlpha(0.92f);
    if (ImGui::Begin("Black Arrow Online Wrong Copy", NULL,
                     ImGuiWindowFlags_NoDecoration |
                     ImGuiWindowFlags_NoFocusOnAppearing |
                     ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 420.0f * scale);
        ImGui::TextColored(problem_color,
                           "Opened from the download folder");
#if defined(__APPLE__)
        ImGui::TextWrapped(
            "Updates go to Black Arrow Online in your Applications folder, "
            "so this copy will stay on this build. Quit, then open Black "
            "Arrow Online from Applications (Finder > Applications, or "
            "Launchpad). You can delete the downloaded folder.");
#else
        ImGui::TextWrapped(
            "Updates go to the Black Arrow Online that setup.sh installed, so "
            "this copy will stay on this build. Quit, then open Black Arrow "
            "Online from your applications menu (or Steam, if you added it "
            "there). You can delete the downloaded folder.");
#endif
        ImGui::PopTextWrapPos();
        PushGreenButtons();
        if (ImGui::Button("OK")) {
            m_wrong_copy = false;
        }
        PopButtonColors();
        PopupStackTake();
    }
    ImGui::End();
}

void SelfUpdateWindow::Open()
{
    is_open = true;
    m_later = false;
    g_mutex_lock(&m_lock);
    State state = m_state;
    g_mutex_unlock(&m_lock);
    if (state != CHECKING && state != DOWNLOADING && state != READY &&
        state != AVAILABLE) {
        Check(false);
    }
}

void SelfUpdateWindow::Check(bool quiet)
{
    g_mutex_lock(&m_lock);
    if (m_state == CHECKING || m_state == DOWNLOADING || m_state == READY) {
        g_mutex_unlock(&m_lock);
        return;
    }
    m_state = CHECKING;
    m_error.clear();
    g_mutex_unlock(&m_lock);
    m_quiet = quiet;
    g_thread_unref(g_thread_new("app-update-check", CheckThread, this));
}

gpointer SelfUpdateWindow::CheckThread(gpointer opaque)
{
    SelfUpdateWindow *self = (SelfUpdateWindow *)opaque;
    GByteArray *data = g_byte_array_new();
    Error *err = NULL;
    int code = http_get((Releases() + "/latest/download/" INFO_NAME).c_str(), data,
                        NULL, &err);
    std::string text((const char *)data->data, data->len);
    g_byte_array_free(data, TRUE);
    int local = LocalBuild();
    int build = atoi(Field(text, "build").c_str());
    std::string sha256 = Field(text, "sha256");

    g_mutex_lock(&self->m_lock);
    self->m_local_build = local;
    if (code == 404) {
        // The newest build has no app for this system (its build failed or
        // is still being made)
        self->m_state = CHECK_FAILED;
        self->m_error = "The newest build has no " PLATFORM " version yet.";
    } else if (code != 200) {
        self->m_state = CHECK_FAILED;
        if (err) {
            self->m_error = error_get_pretty(err);
        } else {
            self->m_error = "GitHub answered " + std::to_string(code) + ".";
        }
    } else if (build <= 0 || sha256.size() != 64) {
        self->m_state = CHECK_FAILED;
        self->m_error = "Could not read " INFO_NAME ".";
    } else if (build > local && !(self->m_quiet && build == SkippedBuild())) {
        // The check at start leaves out a build the player rolled back from
        self->m_state = AVAILABLE;
        self->m_build = build;
        self->m_sha256 = sha256;
        self->m_notes = Field(text, "notes");
    } else {
        self->m_state = UP_TO_DATE;
        self->m_build = build;
    }
    fprintf(stderr, "update: installed build %d, newest %d (%s)\n", local,
            build, self->m_error.empty() ? "ok" : self->m_error.c_str());
    g_mutex_unlock(&self->m_lock);
    if (err) {
        error_free(err);
    }
    return NULL;
}

bool SelfUpdateWindow::InstallBuild(int build)
{
    g_mutex_lock(&m_lock);
    if (m_state == CHECKING || m_state == DOWNLOADING || m_state == READY) {
        g_mutex_unlock(&m_lock);
        return false;
    }
    m_state = CHECKING;
    m_build = build;
    m_error.clear();
    g_mutex_unlock(&m_lock);
    m_quiet = false;
    m_later = false;
    is_open = true;
    g_thread_unref(g_thread_new("app-update-build", BuildInfoThread, this));
    return true;
}

// mac-version.txt of the chosen build, for its SHA-256, then the download
gpointer SelfUpdateWindow::BuildInfoThread(gpointer opaque)
{
    SelfUpdateWindow *self = (SelfUpdateWindow *)opaque;
    g_mutex_lock(&self->m_lock);
    int build = self->m_build;
    g_mutex_unlock(&self->m_lock);

    GByteArray *data = g_byte_array_new();
    Error *err = NULL;
    char *url = g_strdup_printf("%s/download/build-%d/" INFO_NAME,
                                Releases().c_str(), build);
    int code = http_get(url, data, NULL, &err);
    g_free(url);
    std::string text((const char *)data->data, data->len);
    g_byte_array_free(data, TRUE);
    std::string sha256 = Field(text, "sha256");

    g_mutex_lock(&self->m_lock);
    self->m_local_build = LocalBuild();
    bool ok = false;
    if (code == 404) {
        self->m_state = CHECK_FAILED;
        self->m_error = "Build " + std::to_string(build) +
                        " has no " PLATFORM " version.";
    } else if (code != 200) {
        self->m_state = CHECK_FAILED;
        self->m_error = err ? error_get_pretty(err)
                            : "GitHub answered " + std::to_string(code) + ".";
    } else if (atoi(Field(text, "build").c_str()) != build ||
               sha256.size() != 64) {
        self->m_state = CHECK_FAILED;
        self->m_error = "Could not read " INFO_NAME " of build " +
                        std::to_string(build) + ".";
    } else {
        self->m_state = AVAILABLE;
        self->m_sha256 = sha256;
        self->m_notes = Field(text, "notes");
        ok = true;
    }
    g_mutex_unlock(&self->m_lock);
    if (err) {
        error_free(err);
    }
    fprintf(stderr, "update: build %d chosen: %s\n", build,
            ok ? "downloading" : "not available");
    if (ok) {
        self->Download();
    }
    return NULL;
}

void SelfUpdateWindow::Download()
{
    g_mutex_lock(&m_lock);
    if (m_state != AVAILABLE && m_state != DOWNLOAD_FAILED) {
        g_mutex_unlock(&m_lock);
        return;
    }
    m_state = DOWNLOADING;
    m_error.clear();
    m_dlnow = 0;
    m_dltotal = 0;
    m_cancel = false;
    g_mutex_unlock(&m_lock);
    g_thread_unref(g_thread_new("app-update-download", DownloadThread, this));
}

int SelfUpdateWindow::Progress(http_progress_cb_info *info)
{
    SelfUpdateWindow *self = (SelfUpdateWindow *)info->userptr;
    g_mutex_lock(&self->m_lock);
    self->m_dlnow = info->dlnow;
    self->m_dltotal = info->dltotal;
    bool cancel = self->m_cancel;
    g_mutex_unlock(&self->m_lock);
    // Not 0 stops the download
    return cancel ? 1 : 0;
}

gpointer SelfUpdateWindow::DownloadThread(gpointer opaque)
{
    SelfUpdateWindow *self = (SelfUpdateWindow *)opaque;
    g_mutex_lock(&self->m_lock);
    int build = self->m_build;
    std::string sha256 = self->m_sha256;
    g_mutex_unlock(&self->m_lock);

    std::string error, setup;
    GByteArray *data = g_byte_array_new();
    Error *err = NULL;
    http_progress_cb_info info = {};
    info.progress = Progress;
    info.userptr = self;
    // The build named in its version file, not latest: a newer build
    // published meanwhile would not match the SHA-256
    char *url = g_strdup_printf("%s/download/build-%d/" ZIP_NAME,
                                Releases().c_str(), build);
    int code = http_get(url, data, &info, &err);
    g_free(url);

    g_mutex_lock(&self->m_lock);
    bool cancelled = self->m_cancel;
    g_mutex_unlock(&self->m_lock);

    gchar *dir = NULL;
    if (cancelled) {
        // Back to the offer
    } else if (code != 200) {
        error = err ? std::string("The download stopped: ") +
                          error_get_pretty(err)
                    : "The download stopped (GitHub answered " +
                          std::to_string(code) + ").";
    } else {
        gchar *got = g_compute_checksum_for_data(G_CHECKSUM_SHA256, data->data,
                                                 data->len);
        bool same = g_ascii_strcasecmp(got, sha256.c_str()) == 0;
        g_free(got);
        GError *gerr = NULL;
        if (!same) {
            error = "The download was damaged on the way. Try again.";
        } else if (!(dir = g_dir_make_tmp("BlackArrowOnline-update-XXXXXX",
                                          &gerr))) {
            error = std::string("Could not save the update: ") + gerr->message;
            g_clear_error(&gerr);
        } else {
            gchar *zip = g_build_filename(dir, ZIP_NAME, NULL);
            int status = -1;
            if (!g_file_set_contents(zip, (const gchar *)data->data,
                                     data->len, &gerr)) {
                error = std::string("Could not save the update: ") +
                        gerr->message;
                g_clear_error(&gerr);
            } else {
#if defined(__APPLE__)
                // ditto, as Finder unzips: keeps the app's links
                const gchar *argv[] = { "/usr/bin/ditto", "-x", "-k", zip, dir,
                                        NULL };
                GSpawnFlags flags = G_SPAWN_DEFAULT;
#else
                // tar is on every Linux system, unzip is not
                const gchar *argv[] = { "tar", "-xzf", zip, "-C", dir, NULL };
                GSpawnFlags flags = G_SPAWN_SEARCH_PATH;
#endif
                if (!g_spawn_sync(NULL, (gchar **)argv, NULL, flags, NULL,
                                  NULL, NULL, NULL, &status, &gerr)) {
                    g_clear_error(&gerr);
                    status = -1;
                }
                g_unlink(zip);
            }
            g_free(zip);
            gchar *script =
                g_build_filename(dir, "BlackArrowOnline", "setup.sh", NULL);
#if defined(__APPLE__)
            gchar *app = g_build_filename(dir, "BlackArrowOnline",
                                          "Black Arrow Online.app", NULL);
#else
            gchar *app = g_build_filename(dir, "BlackArrowOnline", "app",
                                          "AppRun", NULL);
#endif
            if (error.empty()) {
                if (status != 0 ||
                    !g_file_test(script, G_FILE_TEST_IS_REGULAR) ||
                    !g_file_test(app, G_FILE_TEST_EXISTS)) {
                    error = "Could not unpack the update.";
                } else {
                    setup = script;
                }
            }
            g_free(script);
            g_free(app);
        }
    }
    g_byte_array_free(data, TRUE);
    if (err) {
        error_free(err);
    }
    g_free(dir);

    fprintf(stderr, "update: download of build %d: %s\n", build,
            cancelled ? "cancelled" : error.empty() ? "ready" : error.c_str());
    g_mutex_lock(&self->m_lock);
    if (cancelled) {
        self->m_state = AVAILABLE;
    } else if (!error.empty()) {
        self->m_state = DOWNLOAD_FAILED;
        self->m_error = error;
    } else {
        self->m_state = READY;
        self->m_setup = setup;
    }
    g_mutex_unlock(&self->m_lock);
    return NULL;
}

// Out of the app's session, so closing the app does not end it
static void Detach(gpointer)
{
    setsid();
}

// Runs the update's setup.sh, then makes sure the app opens again. setup.sh
// opens it itself, but macOS ignores that when it still counts the old copy
// as closing, and the setup.sh of an older build (rolling back) cannot be
// fixed, so this part comes from the running build.
#if defined(__APPLE__)
static const char *const relaunch_script = R"SH(
setup="$1"; pid="$2"
name="Black Arrow Online.app"
log="$HOME/Library/Application Support/Black Arrow Online/system/update.log"
running() { pgrep -qf "$name/Contents/MacOS/" 2>/dev/null; }
/bin/bash "$setup" --update "$pid"
if [ -w /Applications ]; then app="/Applications/$name"; else app="$HOME/Applications/$name"; fi
[ -d "$app" ] || app="/Applications/$name"
for try in 1 2 3 4; do
    waited=0
    while [ "$waited" -lt 8 ]; do
        running && exit 0
        sleep 1
        waited=$((waited + 1))
    done
    echo "$(date): not open after the update, opening $app again (try $try)" >> "$log" 2>/dev/null
    if [ "$try" -lt 3 ]; then open "$app"; else open -n "$app"; fi
done
)SH";
#else
// $3 is the folder with app/ and system/. The app counts as open only when a
// process started from app/ runs its xemu, not for any command line that
// names the folder.
static const char *const relaunch_script = R"SH(
setup="$1"; pid="$2"; data="$3"
log="$data/system/update.log"
app="$data/app/AppRun"
running() {
    exe="$(readlink -f "$data/app/usr/bin/xemu" 2>/dev/null)" || return 1
    for p in $(pgrep -f -- "$data/app/" 2>/dev/null); do
        [ "$(readlink -f "/proc/$p/exe" 2>/dev/null)" = "$exe" ] && return 0
    done
    return 1
}
/bin/bash "$setup" --update "$pid"
for try in 1 2 3; do
    waited=0
    while [ "$waited" -lt 8 ]; do
        running && exit 0
        sleep 1
        waited=$((waited + 1))
    done
    echo "$(date): not open after the update, opening $app again (try $try)" >> "$log" 2>/dev/null
    [ -e "$app" ] || exit 1
    setsid "$app" </dev/null >/dev/null 2>&1 &
done
)SH";
#endif

// Starts the update's setup.sh, which waits for the app to close, and closes
// the app
void SelfUpdateWindow::Restart()
{
    m_restarting = true;
    char pid[16];
    snprintf(pid, sizeof(pid), "%d", (int)getpid());
#if defined(__APPLE__)
    const gchar *argv[] = { "/bin/bash", "-c", relaunch_script,
                            "bao-update", m_setup.c_str(), pid, NULL };
#else
    gchar *data = g_path_get_dirname(AppDir().c_str());
    std::string data_dir = data;
    g_free(data);
    const gchar *argv[] = { "/bin/bash", "-c", relaunch_script,
                            "bao-update", m_setup.c_str(), pid,
                            data_dir.c_str(), NULL };
#endif
    GError *gerr = NULL;
    if (!g_spawn_async(NULL, (gchar **)argv, NULL,
                       (GSpawnFlags)(G_SPAWN_STDOUT_TO_DEV_NULL |
                                     G_SPAWN_STDERR_TO_DEV_NULL),
                       Detach, NULL, NULL, &gerr)) {
        g_mutex_lock(&m_lock);
        m_state = DOWNLOAD_FAILED;
        m_error = std::string("Could not start the installer: ") +
                  gerr->message;
        g_mutex_unlock(&m_lock);
        g_clear_error(&gerr);
        m_restarting = false;
        return;
    }
    fprintf(stderr, "update: closing to install build %d\n", m_build);
    ActionShutdown();
}

// What the last update's setup.sh left
void SelfUpdateWindow::ShowLastResult()
{
    std::string failed_path = ConsoleFile("update-failed.txt");
    std::string done_path = ConsoleFile("update-done.txt");
    if (g_file_test(failed_path.c_str(), G_FILE_TEST_EXISTS)) {
        std::string why = ReadText(failed_path);
        while (!why.empty() && (why.back() == '\n' || why.back() == '\r')) {
            why.pop_back();
        }
        g_unlink(failed_path.c_str());
        g_mutex_lock(&m_lock);
        m_state = INSTALL_FAILED;
        m_error = why.empty() ? "Setup stopped." : why;
        g_mutex_unlock(&m_lock);
        is_open = true;
        m_auto_checked = true;
    }
    if (g_file_test(done_path.c_str(), G_FILE_TEST_EXISTS)) {
        int build = atoi(ReadText(done_path).c_str());
        g_unlink(done_path.c_str());
        if (build > 0) {
            char *msg = g_strdup_printf("Updated to build %d", build);
            xemu_queue_notification(msg);
            g_free(msg);
        }
    }
}

void SelfUpdateWindow::DrawContents(State state)
{
    float scale = g_viewport_mgr.m_scale;
    g_mutex_lock(&m_lock);
    int build = m_build;
    int local = m_local_build;
    std::string notes = m_notes;
    std::string error = m_error;
    size_t dlnow = m_dlnow, dltotal = m_dltotal;
    g_mutex_unlock(&m_lock);

    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 420.0f * scale);
    switch (state) {
    case IDLE:
    case CHECKING:
        ImGui::Text("Looking for a newer build...");
        break;
    case UP_TO_DATE:
        if (local > 0) {
            ImGui::Text("You have the newest build (build %d).", local);
        } else {
            ImGui::Text("You have the newest build.");
        }
        break;
    case CHECK_FAILED:
        ImGui::TextColored(problem_color, "Could not look for updates.");
        ImGui::TextWrapped("%s", error.c_str());
        break;
    case AVAILABLE:
    case DOWNLOAD_FAILED:
        ImGui::Text("Black Arrow Online build %d is ready", build);
        if (!notes.empty()) {
            ImGui::TextDisabled("%s", notes.c_str());
        }
        if (state == DOWNLOAD_FAILED) {
            ImGui::TextColored(problem_color, "%s", error.c_str());
        }
        ImGui::TextWrapped("Update now closes Black Arrow Online, installs "
                           "the new build and opens it again. Your console, "
                           "sign-in, saves, settings and games are kept.");
        break;
    case DOWNLOADING: {
        ImGui::Text("Downloading build %d", build);
        char label[48];
        snprintf(label, sizeof(label), "%.0f / %.0f MB", dlnow / 1048576.0,
                 dltotal / 1048576.0);
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, xbox_green_hover);
        ImGui::ProgressBar(dltotal ? (float)dlnow / dltotal : 0.0f,
                           ImVec2(420.0f * scale, 0), label);
        ImGui::PopStyleColor();
        break;
    }
    case READY:
        ImGui::Text("Closing to install build %d. It opens again by itself.",
                    build);
        break;
    case INSTALL_FAILED:
        ImGui::TextColored(problem_color,
                           "The last update could not be installed.");
        ImGui::TextWrapped("%s", error.c_str());
#if defined(__APPLE__)
        ImGui::TextWrapped("To install it by hand, download "
                           ZIP_NAME " from the releases page and run the "
                           "Terminal line from README.txt, as for the first "
                           "install.");
#else
        ImGui::TextWrapped("To install it by hand, download "
                           ZIP_NAME " from the releases page, unpack it and "
                           "run its setup.sh, as for the first install (see "
                           "README.txt).");
#endif
        break;
    }
    ImGui::PopTextWrapPos();

    ImGui::Spacing();
    PushGreenButtons();
    switch (state) {
    case AVAILABLE:
    case DOWNLOAD_FAILED:
        if (ImGui::Button(state == AVAILABLE ? "Update now" : "Try again")) {
            is_open = true;
            Download();
        }
        ImGui::SameLine();
        if (ImGui::Button("Later")) {
            m_later = true;
            is_open = false;
        }
        break;
    case DOWNLOADING:
        if (ImGui::Button("Cancel")) {
            g_mutex_lock(&m_lock);
            m_cancel = true;
            g_mutex_unlock(&m_lock);
        }
        break;
    case CHECK_FAILED:
        if (ImGui::Button("Try again")) {
            Check(false);
        }
        ImGui::SameLine();
        if (ImGui::Button("Close")) {
            is_open = false;
        }
        break;
    case INSTALL_FAILED:
        if (ImGui::Button("Open the releases page")) {
            SDL_OpenURL(RELEASES_PAGE);
        }
        ImGui::SameLine();
        if (ImGui::Button("Close")) {
            is_open = false;
            g_mutex_lock(&m_lock);
            m_state = IDLE;
            g_mutex_unlock(&m_lock);
        }
        break;
    case UP_TO_DATE:
        if (ImGui::Button("Close")) {
            is_open = false;
        }
        break;
    default:
        break;
    }
    PopButtonColors();
}

void SelfUpdateWindow::Draw()
{
    if (!m_started) {
        m_started = true;
        ShowLastResult();
        m_wrong_copy = WrongCopy();
    }
    if (m_wrong_copy) {
        DrawWrongCopy();
    }
    if (!m_auto_checked && ImGui::GetTime() >= AUTO_DELAY) {
        m_auto_checked = true;
        Check(true);
    }

    g_mutex_lock(&m_lock);
    State state = m_state;
    g_mutex_unlock(&m_lock);
    if (state == READY && !m_restarting) {
        Restart();
    }

    if (is_open) {
        // Opened from the menu, or the player clicked Update now
        float scale = g_viewport_mgr.m_scale;
        const ImGuiViewport *vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing,
                                ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(460.0f * scale, 0),
                                 ImGuiCond_Appearing);
        if (ImGui::Begin("Black Arrow Online Update", &is_open,
                         ImGuiWindowFlags_NoCollapse |
                         ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_AlwaysAutoResize)) {
            DrawContents(state);
        }
        ImGui::End();
        if (!is_open) {
            m_later = true;
        }
        return;
    }

    // Found by itself: a panel in the popup column that does not take the
    // game's controls, until Update now or Later
    if (!m_quiet || m_later || (state != AVAILABLE && state != READY)) {
        return;
    }
    PopupStackPlace();
    ImGui::SetNextWindowBgAlpha(0.92f);
    if (ImGui::Begin("Black Arrow Online Update Available", NULL,
                     ImGuiWindowFlags_NoDecoration |
                     ImGuiWindowFlags_NoFocusOnAppearing |
                     ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_AlwaysAutoResize)) {
        DrawContents(state);
        PopupStackTake();
    }
    ImGui::End();
}
