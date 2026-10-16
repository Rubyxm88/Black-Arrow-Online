//
// xemu User Interface: Versions & Updates window and the build badge
//
// Lists the builds on the releases page, newest first, and installs any of
// them: a newer one to update, an older one to go back to it. Pre-releases
// and drafts are left out, so a build is withdrawn by ticking "Set as a
// pre-release" on GitHub. A release whose notes have a line starting with
// "Broken:" is shown greyed out with the rest of that line, and cannot be
// installed.
//
// Installing closes xemu, since its files cannot be replaced while it runs.
// On Windows the launcher next to the system folder does the work
// (--install-build N --wait-pid P: it waits for xemu to close, downloads the
// build's BlackArrowOnline-update.zip, checks it against the SHA-256 in that
// build's version.txt, installs it and starts it). On a Mac the app's own
// updater downloads the build's Mac update and runs its setup.sh.
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#include "common.hh"
#include "versions.hh"
#include "actions.hh"
#include "viewport-manager.hh"
#include "font-manager.hh"
#include "../xemu-settings.h"
#include "../xemu-notifications.h"
#if defined(__APPLE__)
#include "mac-update.hh"
#endif

extern "C" {
#include "qemu/osdep.h"
#include "qemu/http.h"
#include "qapi/error.h"
#include "system/runstate.h"
}

#include <SDL3/SDL.h>
#include <glib/gstdio.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cfloat>
#include <cmath>
#ifndef _WIN32
#include <unistd.h>
#endif

using json = nlohmann::json;

#define RELEASES_PAGE "https://github.com/Rubyxm88/Black-Arrow-Online/releases"
#define RELEASES_API \
    "https://api.github.com/repos/Rubyxm88/Black-Arrow-Online/releases" \
    "?per_page=100"

#if defined(__APPLE__)
#define PLATFORM_ASSET "BlackArrowOnline-mac-update.zip"
#else
#define PLATFORM_ASSET "BlackArrowOnline-update.zip"
#endif

#define LAUNCHER_NAME "Black Arrow Online.exe"

/* The first build with this window; going back further loses it */
#define FIRST_BUILD_WITH_VERSIONS 15

/* Seconds after starting before looking for a newer build */
#define AUTO_DELAY 15.0

static const ImVec4 xbox_green(0.063f, 0.486f, 0.063f, 1.0f);
static const ImVec4 xbox_green_hover(0.10f, 0.62f, 0.10f, 1.0f);
static const ImVec4 xbox_green_active(0.05f, 0.40f, 0.05f, 1.0f);
static const ImVec4 problem_color(1.0f, 0.55f, 0.35f, 1.0f);
static const ImVec4 update_color(1.0f, 0.74f, 0.20f, 1.0f);

VersionsWindow versions_window;

// --- the installed build ---

struct LocalVersion {
    int build;
    std::string xemu, date, commit;
};

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

// version.txt in the system folder; build 0 for a test build
static const LocalVersion &Installed()
{
    static LocalVersion v;
    static bool read;
    if (!read) {
        read = true;
        gchar *path = g_build_filename(xemu_settings_get_base_path(),
                                       "version.txt", NULL);
        gchar *text = NULL;
        if (g_file_get_contents(path, &text, NULL, NULL)) {
            std::string t(text);
            v.build = atoi(Field(t, "build").c_str());
            v.xemu = Field(t, "xemu");
            v.date = Field(t, "date");
            v.commit = Field(t, "commit").substr(0, 10);
            g_free(text);
        }
        g_free(path);
    }
    return v;
}

static bool UpdateChecksOff()
{
    gchar *path = g_build_filename(xemu_settings_get_base_path(),
                                   "no-update-check.txt", NULL);
    bool off = g_file_test(path, G_FILE_TEST_EXISTS);
    g_free(path);
    return off;
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

static void ParseNotes(const std::string &body, ReleaseBuild &b);

// --- the releases page ---

VersionsWindow::VersionsWindow()
{
    g_mutex_init(&m_lock);
    is_open = false;
    m_state = IDLE;
    m_auto_checked = false;
    m_loaded_time = -1;
    m_confirm = 0;
    m_installing = false;
}

void VersionsWindow::Open()
{
    is_open = true;
    m_confirm = 0;
    m_installing = false;
    m_install_error.clear();
    g_mutex_lock(&m_lock);
    State state = m_state;
    g_mutex_unlock(&m_lock);
    // A list from the last minutes is still good
    if (state == IDLE || state == FAILED ||
        (state == LOADED &&
         g_get_monotonic_time() - m_loaded_time > 300 * G_USEC_PER_SEC)) {
        Load("opened");
    }
}

void VersionsWindow::AutoCheck()
{
    if (m_auto_checked || ImGui::GetTime() < AUTO_DELAY) {
        return;
    }
    m_auto_checked = true;
    if (!UpdateChecksOff()) {
        Load("start");
    }
}

int VersionsWindow::NewestBuild()
{
    int newest = 0;
    g_mutex_lock(&m_lock);
    for (const ReleaseBuild &b : m_builds) {
        if (b.broken.empty()) {
            newest = b.build;
            break;
        }
    }
    g_mutex_unlock(&m_lock);
    return newest;
}

void VersionsWindow::Load(const char *why)
{
    g_mutex_lock(&m_lock);
    if (m_state == LOADING) {
        g_mutex_unlock(&m_lock);
        return;
    }
    m_state = LOADING;
    g_mutex_unlock(&m_lock);
    fprintf(stderr, "versions: getting the list (%s)\n", why);
    g_thread_unref(g_thread_new("versions", LoadThread, this));
}

gpointer VersionsWindow::LoadThread(gpointer opaque)
{
    VersionsWindow *self = (VersionsWindow *)opaque;
    GByteArray *data = g_byte_array_new();
    Error *err = NULL;
    int code = http_get(RELEASES_API, data, NULL, &err);
    std::vector<ReleaseBuild> builds;
    std::string error;

    if (code != 200) {
        error = err ? std::string("Could not reach GitHub: ") +
                          error_get_pretty(err)
                    : "GitHub answered " + std::to_string(code) +
                          (code == 403 ? " (asked too often; try again in an "
                                         "hour)." : ".");
    } else {
        try {
            json releases = json::parse(
                std::string((const char *)data->data, data->len));
            for (const json &r : releases) {
                std::string tag = r.value("tag_name", "");
                // Withdrawn builds are pre-releases; drafts are not out yet
                if (r.value("draft", false) || r.value("prerelease", false) ||
                    tag.rfind("build-", 0) != 0) {
                    continue;
                }
                int build = atoi(tag.c_str() + 6);
                if (build <= 0 || tag != "build-" + std::to_string(build)) {
                    continue;
                }
                bool has_asset = false;
                if (r.contains("assets") && r["assets"].is_array()) {
                    for (const json &a : r["assets"]) {
                        if (a.value("name", "") == PLATFORM_ASSET) {
                            has_asset = true;
                        }
                    }
                }
                if (!has_asset) {
                    continue;
                }
                ReleaseBuild b;
                b.build = build;
                b.date = r.value("published_at", "").substr(0, 10);
                b.title = r.value("name", "");
                std::string body = r["body"].is_string()
                                       ? r["body"].get<std::string>()
                                       : "";
                ParseNotes(body, b);
                builds.push_back(b);
            }
            std::sort(builds.begin(), builds.end(),
                      [](const ReleaseBuild &a, const ReleaseBuild &b) {
                          return a.build > b.build;
                      });
            if (builds.empty()) {
                error = "No builds for this computer on the releases page.";
            }
        } catch (const json::exception &e) {
            error = "Could not read the list of builds.";
        }
    }
    g_byte_array_free(data, TRUE);
    if (err) {
        error_free(err);
    }

    fprintf(stderr, "versions: %d builds on the releases page%s%s\n",
            (int)builds.size(), error.empty() ? "" : ": ", error.c_str());
    g_mutex_lock(&self->m_lock);
    if (error.empty()) {
        self->m_builds = builds;
        self->m_state = LOADED;
        self->m_loaded_time = g_get_monotonic_time();
    } else {
        self->m_error = error;
        self->m_state = FAILED;
    }
    g_mutex_unlock(&self->m_lock);
    return NULL;
}

// Markdown of the release notes, as plain text: no emphasis, code marks or
// links
static std::string PlainText(std::string line)
{
    std::string out;
    for (size_t i = 0; i < line.size(); i++) {
        char c = line[i];
        if (c == '*' || c == '`' || c == '_') {
            continue;
        }
        if (c == '[') {
            size_t close = line.find("](", i);
            size_t paren = close == std::string::npos
                               ? std::string::npos
                               : line.find(')', close);
            if (paren != std::string::npos) {
                out += line.substr(i + 1, close - i - 1);
                i = paren;
                continue;
            }
        }
        out += c;
    }
    return out;
}

// The patch notes are the lists under "What changed", and under headings
// such as "Fixed", "Known problems" or "Patch notes" added by hand; how to
// install and the file list are left out. A line "Broken: why" anywhere
// marks the build as broken.
static void ParseNotes(const std::string &body, ReleaseBuild &b)
{
    bool in_notes = false;
    size_t pos = 0;
    while (pos < body.size()) {
        size_t end = body.find('\n', pos);
        if (end == std::string::npos) {
            end = body.size();
        }
        std::string line = body.substr(pos, end - pos);
        pos = end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
            line.pop_back();
        }
        size_t first = line.find_first_not_of(' ');
        if (first == std::string::npos) {
            continue;
        }
        line = line.substr(first);

        if (line[0] == '#') {
            std::string heading = PlainText(line.substr(
                line.find_first_not_of("# ") == std::string::npos
                    ? line.size()
                    : line.find_first_not_of("# ")));
            gchar *lower = g_ascii_strdown(heading.c_str(), -1);
            in_notes = strstr(lower, "change") || strstr(lower, "fix") ||
                       strstr(lower, "new in") || strstr(lower, "known") ||
                       strstr(lower, "broken") || strstr(lower, "notes");
            g_free(lower);
            if (in_notes && g_ascii_strcasecmp(heading.c_str(),
                                               "What changed") != 0) {
                b.changes.push_back("## " + heading);
            }
            continue;
        }

        std::string text = line;
        bool bullet = false;
        if (text.size() > 1 && (text[0] == '-' || text[0] == '*' ||
                                text[0] == '+') && text[1] == ' ') {
            text = text.substr(2);
            bullet = true;
        }
        text = PlainText(text);
        if (g_ascii_strncasecmp(text.c_str(), "broken:", 7) == 0) {
            size_t s = text.find_first_not_of(' ', 7);
            b.broken = s == std::string::npos ? "Marked as broken."
                                              : text.substr(s);
            continue;
        }
        if (b.notes.empty() && !bullet && line[0] != '|' && line[0] != '<') {
            b.notes = text;  // "Build N of Black Arrow Online, based on..."
        }
        if (in_notes && line[0] != '|' && line[0] != '<') {
            b.changes.push_back(bullet ? text : "  " + text);
        }
    }
}

// --- installing ---

void VersionsWindow::Install(int build)
{
#if defined(_WIN32)
    // The launcher next to the system folder installs it once xemu closed
    gchar *base = g_strdup(xemu_settings_get_base_path());
    size_t n = strlen(base);
    while (n > 0 && (base[n - 1] == '/' || base[n - 1] == '\\')) {
        base[--n] = 0;
    }
    gchar *root = g_path_get_dirname(base);
    gchar *launcher = g_build_filename(root, LAUNCHER_NAME, NULL);
    g_free(base);
    if (!g_file_test(launcher, G_FILE_TEST_IS_REGULAR)) {
        m_install_error = "The launcher (" LAUNCHER_NAME ") is not next to "
                          "the system folder. Start Black Arrow Online from "
                          "it to change builds.";
        g_free(launcher);
        g_free(root);
        return;
    }
    char build_arg[16], pid_arg[16];
    snprintf(build_arg, sizeof(build_arg), "%d", build);
    snprintf(pid_arg, sizeof(pid_arg), "%lu",
             (unsigned long)GetCurrentProcessId());
    const gchar *argv[] = { launcher, "--install-build", build_arg,
                            "--wait-pid", pid_arg, NULL };
    GError *gerr = NULL;
    if (!g_spawn_async(root, (gchar **)argv, NULL, G_SPAWN_DEFAULT, NULL,
                       NULL, NULL, &gerr)) {
        m_install_error = std::string("Could not start the launcher: ") +
                          gerr->message;
        g_clear_error(&gerr);
        g_free(launcher);
        g_free(root);
        return;
    }
    g_free(launcher);
    g_free(root);
    fprintf(stderr, "versions: closing for the launcher to install build %d\n",
            build);
    m_installing = true;
    ActionShutdown();
#elif defined(__APPLE__)
    // The app's updater downloads it, then its setup.sh installs it once the
    // app closed
    if (!mac_update_window.InstallBuild(build)) {
        m_install_error = "An update is already being installed.";
        return;
    }
    m_installing = true;
    is_open = false;
#else
    (void)build;
    m_install_error = "Builds are installed from the releases page on this "
                      "system.";
    SDL_OpenURL(RELEASES_PAGE);
#endif
}

void VersionsWindow::DrawConfirm()
{
    if (m_confirm && !ImGui::IsPopupOpen("Install this build?")) {
        ImGui::OpenPopup("Install this build?");
    }
    float scale = g_viewport_mgr.m_scale;
    ImGui::SetNextWindowSize(ImVec2(470.0f * scale, 0), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Install this build?", NULL,
                                ImGuiWindowFlags_NoSavedSettings |
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    int build = m_confirm;
    int local = Installed().build;
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 450.0f * scale);
    ImGui::PushFont(g_font_mgr.m_menu_font_medium);
    if (local > 0 && build < local) {
        ImGui::Text("Go back to build %d?", build);
    } else if (local > 0 && build == local) {
        ImGui::Text("Install build %d again?", build);
    } else {
        ImGui::Text("Update to build %d?", build);
    }
    ImGui::PopFont();
    ImGui::Spacing();
    ImGui::TextWrapped("Black Arrow Online closes, installs build %d and "
                       "opens again by itself. Your console, Xbox Live "
                       "sign-in, saves, settings and games are kept.",
                       build);
    if (runstate_is_running() && g_config.sys.files.dvd_path &&
        g_config.sys.files.dvd_path[0]) {
        ImGui::TextColored(problem_color, "A game is running: anything not "
                                          "saved in it is lost.");
    }
    if (build < FIRST_BUILD_WITH_VERSIONS) {
        ImGui::TextColored(update_color, "Build %d does not have this window "
                           "yet.", build);
#if defined(__APPLE__)
        ImGui::TextWrapped("To come back to a newer build, choose Update now "
                           "when the app offers it after starting.");
#else
        ImGui::TextWrapped("To come back to a newer build, choose Update now "
                           "when the launcher offers it at the start.");
#endif
    }
    ImGui::PopTextWrapPos();
    ImGui::Spacing();

    PushGreenButtons();
    if (ImGui::Button("Close and Install")) {
        m_confirm = 0;
        ImGui::CloseCurrentPopup();
        Install(build);
    }
    PopButtonColors();
    ImGui::SameLine();
    if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        m_confirm = 0;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// --- the window ---

void VersionsWindow::Draw()
{
    if (!is_open) {
        return;
    }

    float scale = g_viewport_mgr.m_scale;
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(640.0f * scale, 470.0f * scale),
                             ImGuiCond_Appearing);
    if (!ImGui::Begin("Versions & Updates", &is_open,
                      ImGuiWindowFlags_NoCollapse |
                      ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::End();
        return;
    }

    g_mutex_lock(&m_lock);
    State state = m_state;
    std::vector<ReleaseBuild> builds = m_builds;
    std::string error = m_error;
    g_mutex_unlock(&m_lock);

    // What is installed
    const LocalVersion &local = Installed();
    int newest = NewestBuild();
    ImGui::PushFont(g_font_mgr.m_menu_font_medium);
    if (local.build > 0) {
        ImGui::Text("Build %d", local.build);
    } else {
        ImGui::Text("Test build");
    }
    ImGui::PopFont();
    ImGui::SameLine();
    if (newest > local.build && local.build > 0) {
        ImGui::TextColored(update_color, "  Build %d is out", newest);
    } else if (newest > 0 && newest == local.build) {
        ImGui::TextColored(ImVec4(0.45f, 0.9f, 0.45f, 1.0f), "  Up to date");
    }
    std::string details;
    if (!local.xemu.empty()) {
        details += "xemu " + local.xemu;
    }
    if (!local.date.empty()) {
        details += (details.empty() ? "" : "  -  ") + local.date;
    }
    if (!local.commit.empty()) {
        details += (details.empty() ? "" : "  -  ") + local.commit;
    }
    if (!details.empty()) {
        ImGui::TextDisabled("%s", details.c_str());
    }
    ImGui::TextWrapped("Install any build from the releases page: a newer "
                       "one to update, an older one to go back to it if the "
                       "newest gives you trouble.");
    if (!m_install_error.empty()) {
        ImGui::TextColored(problem_color, "%s", m_install_error.c_str());
    }
    ImGui::Separator();

    // The builds
    float bottom = ImGui::GetFrameHeightWithSpacing() + 4.0f * scale;
    ImGui::BeginChild("builds", ImVec2(0, -bottom), false);
    if (state == LOADING && builds.empty()) {
        ImGui::TextDisabled("Getting the list of builds...");
    } else if (state == FAILED && builds.empty()) {
        ImGui::TextColored(problem_color, "%s", error.c_str());
    }
    for (const ReleaseBuild &b : builds) {
        ImGui::PushID(b.build);
        bool installed = b.build == local.build;
        bool broken = !b.broken.empty();
        ImDrawList *dl = ImGui::GetWindowDrawList();
        ImVec2 row = ImGui::GetCursorScreenPos();
        float width = ImGui::GetContentRegionAvail().x;

        // A heading per build that opens its patch notes; the installed and
        // the newest build start open
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth |
                                   ImGuiTreeNodeFlags_AllowOverlap |
                                   ImGuiTreeNodeFlags_FramePadding;
        if (installed || b.build == newest) {
            flags |= ImGuiTreeNodeFlags_DefaultOpen;
        }
        if (installed) {
            dl->AddRectFilled(row,
                              ImVec2(row.x + width,
                                     row.y + ImGui::GetFrameHeight()),
                              IM_COL32(16, 124, 16, 60), 4.0f * scale);
        }
        if (broken) {
            ImGui::PushStyleColor(ImGuiCol_Text,
                                  ImGui::GetStyleColorVec4(
                                      ImGuiCol_TextDisabled));
        }
        bool open = ImGui::TreeNodeEx("##build", flags, "Build %d", b.build);
        if (broken) {
            ImGui::PopStyleColor();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%s", b.date.c_str());
        if (installed) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.45f, 0.9f, 0.45f, 1.0f), "Installed");
        }
        if (b.build == newest) {
            ImGui::SameLine();
            ImGui::TextColored(update_color, "Newest");
        }
        if (broken) {
            ImGui::SameLine();
            ImGui::TextColored(problem_color, "Broken");
        }

        // The button at the right of the heading
        if (!broken) {
            const char *label = installed                       ? "Reinstall"
                                : local.build > 0 && b.build < local.build
                                    ? "Go back"
                                    : "Update";
            float bw = ImGui::CalcTextSize(label).x +
                       ImGui::GetStyle().FramePadding.x * 2;
            ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - bw);
            bool primary = !installed && b.build == newest;
            if (primary) {
                PushGreenButtons();
            }
            ImGui::BeginDisabled(m_installing);
            if (ImGui::SmallButton(label)) {
                m_confirm = b.build;
                m_install_error.clear();
            }
            ImGui::EndDisabled();
            if (primary) {
                PopButtonColors();
            }
        }

        if (open) {
            ImGui::Indent(4.0f * scale);
            ImGui::PushTextWrapPos(ImGui::GetContentRegionMax().x -
                                   8.0f * scale);
            if (broken) {
                ImGui::TextColored(problem_color, "Do not install: %s",
                                   b.broken.c_str());
            }
            bool any = false;
            for (const std::string &line : b.changes) {
                if (line.rfind("## ", 0) == 0) {
                    ImGui::Spacing();
                    ImGui::TextDisabled("%s", line.c_str() + 3);
                } else if (line.rfind("  ", 0) == 0) {
                    ImGui::Indent(ImGui::GetTreeNodeToLabelSpacing());
                    ImGui::TextWrapped("%s", line.c_str() + 2);
                    ImGui::Unindent(ImGui::GetTreeNodeToLabelSpacing());
                } else {
                    ImGui::Bullet();
                    ImGui::TextWrapped("%s", line.c_str());
                }
                any = true;
            }
            if (!any) {
                ImGui::TextDisabled("%s", b.notes.empty() ? "No notes."
                                                          : b.notes.c_str());
            }
            ImGui::PopTextWrapPos();
            ImGui::Unindent(4.0f * scale);
            ImGui::Spacing();
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    ImGui::EndChild();

    ImGui::Separator();
    ImGui::BeginDisabled(state == LOADING);
    if (ImGui::Button(state == LOADING ? "Refreshing..." : "Refresh")) {
        Load("refresh");
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Releases Page")) {
        SDL_OpenURL(RELEASES_PAGE);
    }
    ImGui::SameLine();
    if (ImGui::Button("Close")) {
        is_open = false;
    }

    DrawConfirm();
    ImGui::End();
}

// --- the badge ---

static float Smooth(float x)
{
    x = x < 0 ? 0 : x > 1 ? 1 : x;
    return x * x * (3 - 2 * x);
}

void DrawBuildBadge()
{
    versions_window.AutoCheck();

    const LocalVersion &local = Installed();
    int newest = versions_window.NewestBuild();
    bool update = local.build > 0 && newest > local.build;
    float scale = g_viewport_mgr.m_scale;
    double t = ImGui::GetTime();
    // The menu bar fades out when the mouse rests; fade with it
    float fade = ImGui::GetStyleColorVec4(ImGuiCol_Text).w;

    // "BA Online" light, " - " dim, "Build N" green
    char build_part[32];
    if (local.build > 0) {
        snprintf(build_part, sizeof(build_part), "Build %d", local.build);
    } else {
        snprintf(build_part, sizeof(build_part), "Test Build");
    }
    std::string text = std::string("BA Online - ") + build_part;
    size_t dim_from = strlen("BA Online"), green_from = strlen("BA Online - ");
    const ImVec4 name_color(0.86f, 0.88f, 0.86f, 1.0f);
    const ImVec4 dash_color(0.50f, 0.53f, 0.50f, 1.0f);
    const ImVec4 build_color(0.36f, 0.82f, 0.38f, 1.0f);

    ImFont *font = ImGui::GetFont();
    float size = ImGui::GetFontSize();
    ImVec2 text_size = ImGui::CalcTextSize(text.c_str());
    float dot = update ? size * 0.18f : 0;
    float gap = update ? size * 0.55f : 0;
    float w = text_size.x + gap + dot * 2;
    float bar_h = ImGui::GetWindowHeight();

    // At the right edge, unless the menus already reach there
    ImVec2 win = ImGui::GetWindowPos();
    float right = win.x + ImGui::GetWindowWidth() - 12.0f * scale;
    float x = std::max(right - w, ImGui::GetCursorScreenPos().x + 12.0f * scale);
    ImVec2 p(x, win.y + (bar_h - text_size.y) / 2);
    ImGui::SetCursorScreenPos(ImVec2(p.x, win.y));
    bool clicked = ImGui::InvisibleButton("##build-badge", ImVec2(w, bar_h));
    bool hovered = ImGui::IsItemHovered();
    ImDrawList *dl = ImGui::GetWindowDrawList();

    // Hovering eases in over a fifth of a second
    static float hover_amount;
    float step = ImGui::GetIO().DeltaTime * 5.0f;
    hover_amount = hovered ? std::min(1.0f, hover_amount + step)
                           : std::max(0.0f, hover_amount - step);
    float h = Smooth(hover_amount);

    // A soft light passes over the letters every 10 seconds, taking 2.4
    const double period = 10.0, sweep = 2.4;
    double phase = fmod(t + 3.0, period);
    float light_x = -1e9f;
    if (phase < sweep) {
        float k = Smooth((float)(phase / sweep));
        light_x = p.x - size * 2 + (text_size.x + size * 4) * k;
    }
    float radius = size * 1.6f;

    float cx = p.x;
    for (size_t i = 0; i < text.size(); i++) {
        const char *c = text.c_str() + i;
        float advance = font->CalcTextSizeA(size, FLT_MAX, 0, c, c + 1).x;
        ImVec4 col = i >= green_from ? build_color
                     : i >= dim_from ? dash_color
                                     : name_color;
        float d = (cx + advance / 2 - light_x) / radius;
        float light = expf(-d * d) * 0.75f;
        float lift = std::max(light, h * 0.6f);
        col.x += (1.0f - col.x) * lift;
        col.y += (1.0f - col.y) * lift;
        col.z += (1.0f - col.z) * lift;
        col.w = fade;
        dl->AddText(font, size, ImVec2(cx, p.y), ImGui::GetColorU32(col), c,
                    c + 1);
        cx += advance;
    }

    // Under the mouse, a thin green line grows out from the middle
    if (h > 0.01f) {
        float mid = p.x + text_size.x / 2;
        float half = text_size.x / 2 * h;
        float y = p.y + text_size.y + 1.5f * scale;
        dl->AddLine(ImVec2(mid - half, y), ImVec2(mid + half, y),
                    ImGui::GetColorU32(ImVec4(build_color.x, build_color.y,
                                              build_color.z, fade * h)),
                    1.2f * scale);
    }

    // A newer build is out: an amber dot that breathes, with a ring
    if (update) {
        ImVec2 c(p.x + text_size.x + gap + dot, p.y + text_size.y / 2);
        float pulse = (float)fmod(t, 2.4) / 2.4f;
        dl->AddCircle(c, dot * (1.0f + pulse * 1.8f),
                      ImGui::GetColorU32(ImVec4(update_color.x, update_color.y,
                                                update_color.z,
                                                fade * 0.6f * (1 - pulse))),
                      0, 1.0f * scale);
        dl->AddCircleFilled(c, dot,
                            ImGui::GetColorU32(ImVec4(update_color.x,
                                                      update_color.y,
                                                      update_color.z, fade)));
    }

    if (hovered) {
        ImGui::BeginTooltip();
        if (local.build > 0) {
            ImGui::Text("Black Arrow Online build %d", local.build);
        } else {
            ImGui::Text("Black Arrow Online test build");
        }
        if (!local.xemu.empty()) {
            ImGui::TextDisabled("xemu %s", local.xemu.c_str());
        }
        if (!local.date.empty()) {
            ImGui::TextDisabled("Built %s", local.date.c_str());
        }
        if (update) {
            ImGui::TextColored(update_color, "Build %d is out", newest);
        }
        ImGui::TextDisabled("Click for versions and updates");
        ImGui::EndTooltip();
    }
    if (clicked) {
        versions_window.Open();
    }
}
