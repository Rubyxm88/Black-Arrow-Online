//
// xemu User Interface
//
// Copyright (C) 2020-2022 Matt Borgerson
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
#include "ui/xemu-notifications.h"
#include "common.hh"
#include "main-menu.hh"
#include "menubar.hh"
#include "misc.hh"
#include "widgets.hh"
#include "monitor.hh"
#include "debug.hh"
#include "actions.hh"
#include "compat.hh"
#include "update.hh"
#include "xbcheck.hh"
#include "shader-cache.hh"
#include "../xemu-os-utils.h"
#include "../xemu-xbonline.h"
#include "../xemu-xbcheck.h"
#include "../xemu-diag.h"
#include "hw/xbox/nv2a/shader-precache.h"
#include <filesystem>
#include <map>
#include <string>

extern float g_main_menu_height; // FIXME

#ifdef CONFIG_RENDERDOC
bool g_capture_renderdoc_frame = false;
#endif

#if defined(__APPLE__)
#define SHORTCUT_MENU_TEXT(c) "Cmd+" #c
#else
#define SHORTCUT_MENU_TEXT(c) "Ctrl+" #c
#endif

void ProcessKeyboardShortcuts(void)
{
    if (IsShortcutKeyPressed(ImGuiKey_E)) {
        ActionEjectDisc();
    }

    if (IsShortcutKeyPressed(ImGuiKey_O)) {
        ActionLoadDisc();
    }

    if (IsShortcutKeyPressed(ImGuiKey_P)) {
        ActionTogglePause();
    }

    if (IsShortcutKeyPressed(ImGuiKey_R)) {
        ActionReset();
    }

    if (IsShortcutKeyPressed(ImGuiKey_Q)) {
        ActionShutdown();
    }

    if (ImGui::IsKeyPressed(ImGuiKey_GraveAccent)) {
        monitor_window.ToggleOpen();
    }

    if (ImGui::IsKeyPressed(ImGuiKey_F12)) {
        ActionScreenshot();
    }

    if (ImGui::IsKeyPressed(ImGuiKey_F9, false) && xemu_diag_active()) {
        xemu_diag_mark();
        xemu_queue_notification("Lag spike marked in the diagnostics log");
    }

    if (ImGui::IsKeyPressed(ImGuiKey_F11)) {
        xemu_toggle_fullscreen();
    }

#ifdef CONFIG_RENDERDOC
    if (ImGui::IsKeyPressed(ImGuiKey_F10) && nv2a_dbg_renderdoc_available()) {
        ImGuiIO& io = ImGui::GetIO();
        int num_frames = io.KeyShift ? 5 : 1;
        nv2a_dbg_renderdoc_capture_frames(num_frames, io.KeyCtrl);
    }
#endif
}

// Section title inside a menu, in Xbox green
static void MenuHeader(const char *title)
{
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.42f, 0.80f, 0.30f, 1.0f));
    ImGui::SeparatorText(title);
    ImGui::PopStyleColor();
}

// A menu item with a check mark that flips a setting; hovering explains it
static void MenuToggle(const char *label, bool *value, const char *help)
{
    ImGui::MenuItem(label, NULL, value);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort |
                             ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.0f);
        ImGui::TextUnformatted(help);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

static void StartDiagnosticsLog()
{
    if (xemu_diag_start()) {
        xemu_queue_notification("Recording a diagnostics log. "
                                "Press F9 when the game lags.");
    } else {
        xemu_queue_error_message("Could not create the diagnostics log file.");
    }
}

static void StopDiagnosticsLog()
{
    xemu_diag_stop();
    xemu_queue_notification("Diagnostics log saved in the logs folder");
    xemu_diag_open_folder();
}

void ShowMainMenu()
{
    bool running = runstate_is_running();

    if (ImGui::BeginMainMenuBar())
    {
        if (ImGui::BeginMenu("Machine"))
        {
            if (ImGui::MenuItem(running ? "Pause" : "Resume", SHORTCUT_MENU_TEXT(P))) ActionTogglePause();
            if (ImGui::MenuItem("Screenshot", "F12")) ActionScreenshot();

            if (ImGui::BeginMenu("Snapshot")) {
                if (ImGui::MenuItem("Create Snapshot")) {
                    xemu_snapshots_save(NULL, NULL);
                    xemu_queue_notification("Created new snapshot");
                }

                for (int i = 0; i < 4; ++i) {
                    char *hotkey = g_strdup_printf("Shift+F%d", i + 5);

                    char *load_name;
                    char *save_name;

                    assert(g_snapshot_shortcut_index_key_map[i]);
                    bool bound = *(g_snapshot_shortcut_index_key_map[i]) &&
                            (**(g_snapshot_shortcut_index_key_map[i]) != 0);

                    if (bound) {
                        load_name = g_strdup_printf("Load '%s'", *(g_snapshot_shortcut_index_key_map[i]));
                        save_name = g_strdup_printf("Save '%s'", *(g_snapshot_shortcut_index_key_map[i]));
                    } else {
                        load_name = g_strdup_printf("Load F%d (Unbound)", i + 5);
                        save_name = g_strdup_printf("Save F%d (Unbound)", i + 5);
                    }

                    ImGui::Separator();

                    if (ImGui::MenuItem(load_name, hotkey + sizeof("Shift+") - 1, false, bound)) {
                        ActionActivateBoundSnapshot(i, false);
                    }

                    if (ImGui::MenuItem(save_name, hotkey, false, bound)) {
                        ActionActivateBoundSnapshot(i, true);
                    }

                    g_free(hotkey);
                    g_free(load_name);
                    g_free(save_name);
                }

                ImGui::EndMenu();
            }

            ImGui::Separator();

            if (ImGui::MenuItem("Eject Disc", SHORTCUT_MENU_TEXT(E))) ActionEjectDisc();
            if (ImGui::MenuItem("Load Disc...", SHORTCUT_MENU_TEXT(O))) ActionLoadDisc();

            ImGui::Separator();

            if (ImGui::MenuItem("Settings...")) g_main_menu.ShowSettings();

            ImGui::Separator();

            if (ImGui::MenuItem("Reset", SHORTCUT_MENU_TEXT(R))) ActionReset();
            if (ImGui::MenuItem("Exit", SHORTCUT_MENU_TEXT(Q))) ActionShutdown();
            ImGui::EndMenu();
        }

        static bool xbonline_menu_open;
        if (ImGui::BeginMenu("XB Online"))
        {
            // Read the accounts once each time the menu opens
            static char account_names[128];
            static int account_count = -1;
            if (!xbonline_menu_open) {
                account_count = xemu_xbonline_hdd_account_names(
                    g_config.sys.files.hdd_path, account_names,
                    sizeof(account_names));
            }
            xbonline_menu_open = true;

            std::multimap<std::string, std::string> games;
            std::filesystem::path games_dir(g_config.general.games_dir);
            std::error_code ec;
            if (g_config.general.games_dir[0] &&
                std::filesystem::is_directory(games_dir, ec)) {
                for (const auto &file :
                     std::filesystem::directory_iterator(games_dir, ec)) {
                    const auto &path = file.path();
                    if (path.extension() == ".iso" ||
                        path.extension() == ".xiso") {
                        games.insert({ path.stem().string(), path.string() });
                    }
                }
            }

            MenuHeader("Games");
            std::filesystem::path current(g_config.sys.files.dvd_path);
            for (const auto &[label, path] : games) {
                std::string item = "Play " + label;
                bool loaded = std::filesystem::equivalent(current, path, ec);
                if (ImGui::MenuItem(item.c_str(), NULL, loaded)) {
                    ActionPlayDiscFile(path.c_str());
                }
            }
            if (games.empty() &&
                ImGui::MenuItem("No games yet: Open Games Folder")) {
                xemu_xbcheck_open_games_folder();
            }
            if (qemu_access(XEMU_XBONLINE_UPDATE_DISC, F_OK) == 0 &&
                ImGui::MenuItem("Black Arrow - Install Game Updates")) {
                ActionPlayDiscFile(XEMU_XBONLINE_UPDATE_DISC);
            }

            MenuHeader("Dashboards");
            if (ImGui::MenuItem("XBL Dashboard")) ActionXboxLiveSetup();
            if (ImGui::MenuItem("Original Xbox Dashboard")) ActionXboxDashboard();

            MenuHeader("XB Live Account");
            if (account_count > 0) {
                std::string item = std::string(account_count > 1 ?
                                               "Accounts: " : "Account: ") +
                                   account_names;
                if (ImGui::MenuItem(item.c_str())) {
                    SDL_OpenURL(XEMU_XBONLINE_URL_DASHBOARD);
                }
            } else if (ImGui::MenuItem("No account yet: add it in XBL Dashboard")) {
                ActionXboxLiveSetup();
            }
            if (ImGui::MenuItem("Create an Account")) {
                SDL_OpenURL(XEMU_XBONLINE_URL_CREATE);
            }
            if (ImGui::MenuItem("Link Code")) {
                SDL_OpenURL(XEMU_XBONLINE_URL_LINK);
            }

            MenuHeader("Help");
            if (ImGui::MenuItem("Connection Check")) {
                xbcheck_window.Start(true);
            }
            if (xemu_diag_active()) {
                int secs = (int)xemu_diag_seconds();
                char time[16];
                snprintf(time, sizeof(time), "%d:%02d", secs / 60, secs % 60);
                if (ImGui::MenuItem("Stop Diagnostics Log", time)) {
                    StopDiagnosticsLog();
                }
                if (ImGui::MenuItem("Mark Lag Spike", "F9")) {
                    xemu_diag_mark();
                }
            } else if (ImGui::MenuItem("Record Diagnostics Log")) {
                StartDiagnosticsLog();
            }
            if (ImGui::MenuItem("Open Logs Folder")) {
                xemu_diag_open_folder();
            }
            if (ImGui::MenuItem("Shader Cache")) {
                shader_cache_window.Open();
            }
            ImGui::EndMenu();
        } else {
            xbonline_menu_open = false;
        }

        if (ImGui::BeginMenu("RSO Settings"))
        {
            ShaderPrecacheStatus shader_status;
            nv2a_shader_precache_get_status(&shader_status);
            bool opengl_running = shader_status.supported;

            MenuHeader("Shaders");
            if (ImGui::MenuItem("Shader Cache...")) {
                shader_cache_window.Open();
            }
            MenuToggle("Save shaders to disk", &g_config.perf.cache_shaders,
                       "Keep every shader the game builds, so it never has to "
                       "build it again on this PC.");
            ImGui::BeginDisabled(!opengl_running);
            MenuToggle("Build shaders in the background",
                       &g_config.perf.shader_async,
                       "Instead of freezing while a new shader builds, skip "
                       "that effect for a few frames. Turn off if effects "
                       "flicker. OpenGL only.");
            ImGui::EndDisabled();
            MenuToggle("Precache shaders automatically",
                       &g_config.perf.shader_precache_auto,
                       "Build missing shaders in the background by "
                       "themselves at start (first run, after a graphics "
                       "driver update or a resolution change), with a small "
                       "notice. Turn off to only precache from Shader "
                       "Cache...");
            MenuToggle("Precache this PC's own shaders",
                       &g_config.perf.shader_precache_own,
                       "Precache Shaders also rebuilds every shader this PC "
                       "has used before, not only the Black Arrow list.");

            MenuHeader("Graphics");
            if (ImGui::BeginMenu("Renderer")) {
                if (ImGui::MenuItem("OpenGL (recommended)", NULL,
                                    g_config.display.renderer ==
                                        CONFIG_DISPLAY_RENDERER_OPENGL)) {
                    g_config.display.renderer = CONFIG_DISPLAY_RENDERER_OPENGL;
                }
#ifdef CONFIG_VULKAN
                if (ImGui::MenuItem("Vulkan", NULL,
                                    g_config.display.renderer ==
                                        CONFIG_DISPLAY_RENDERER_VULKAN)) {
                    g_config.display.renderer = CONFIG_DISPLAY_RENDERER_VULKAN;
                }
#endif
                ImGui::EndMenu();
            }
            if (opengl_running != (g_config.display.renderer ==
                                   CONFIG_DISPLAY_RENDERER_OPENGL)) {
                ImGui::TextDisabled("  Restart Black Arrow Online to switch "
                                    "renderer");
            }
            int scale = nv2a_get_surface_scale_factor();
            char scale_label[32];
            snprintf(scale_label, sizeof(scale_label), "Resolution: %dx", scale);
            if (ImGui::BeginMenu(scale_label)) {
                static const char *const scales[] = {
                    "1x (640x480, fastest)", "2x (1280x960)", "3x (1920x1440)",
                    "4x (2560x1920, slowest)"
                };
                for (int i = 0; i < 4; i++) {
                    if (ImGui::MenuItem(scales[i], NULL, scale == i + 1)) {
                        nv2a_set_surface_scale_factor(i + 1);
                    }
                }
                ImGui::EndMenu();
            }
            MenuToggle("Skip render target copy-back",
                       &g_config.display.quality.skip_reuse_copyback,
                       "Faster when the game reuses a screen buffer every "
                       "frame. Turn off if effects look wrong. OpenGL only.");
            MenuToggle("Vertical sync", &g_config.display.window.vsync,
                       "Stops screen tearing. Turning it off can lower input "
                       "delay but may tear.");

            MenuHeader("Diagnostics");
            if (xemu_diag_active()) {
                if (ImGui::MenuItem("Stop Diagnostics Log")) {
                    StopDiagnosticsLog();
                }
            } else if (ImGui::MenuItem("Record Diagnostics Log")) {
                StartDiagnosticsLog();
            }
            if (ImGui::MenuItem("Open Logs Folder")) {
                xemu_diag_open_folder();
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("View"))
        {
            int ui_scale_idx;
            if (g_config.display.ui.auto_scale) {
                ui_scale_idx = 0;
            } else {
                ui_scale_idx = g_config.display.ui.scale;
                if (ui_scale_idx < 1) ui_scale_idx = 1;
                else if (ui_scale_idx > 2) ui_scale_idx = 2;
            }
            if (ImGui::Combo("UI Scale", &ui_scale_idx,
                             "Auto\0"
                             "1x\0"
                             "2x\0")) {
                if (ui_scale_idx == 0) {
                    g_config.display.ui.auto_scale = true;
                } else {
                    g_config.display.ui.auto_scale = false;
                    g_config.display.ui.scale = ui_scale_idx;
                }
            }

            ImGui::Combo("Backend", &g_config.display.renderer,
                 "Null\0"
                 "OpenGL\0"
#ifdef CONFIG_VULKAN
                 "Vulkan\0"
#endif
                );

            int rendering_scale = nv2a_get_surface_scale_factor() - 1;
            if (ImGui::Combo("Int. Resolution Scale", &rendering_scale,
                             "1x\0"
                             "2x\0"
                             "3x\0"
                             "4x\0"
                             "5x\0"
                             "6x\0"
                             "7x\0"
                             "8x\0"
                             "9x\0"
                             "10x\0")) {
                nv2a_set_surface_scale_factor(rendering_scale + 1);
            }

            ImGui::Combo("Display Mode", &g_config.display.ui.fit,
                         "Center\0Scale\0Stretch\0");
            ImGui::SameLine();
            HelpMarker("Controls how the rendered content should be scaled "
                       "into the window");
            ImGui::Combo("Filter Method", &g_config.display.filtering,
                         "Linear\0Nearest\0");
            ImGui::Combo("Aspect Ratio", &g_config.display.ui.aspect_ratio,
                         "Native\0Auto\0""4:3\0""16:9\0");
            if (ImGui::MenuItem("Fullscreen", "F11",
                                xemu_is_fullscreen(), true)) {
                xemu_toggle_fullscreen();
            }

            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Debug"))
        {
            ImGui::MenuItem("Monitor", "~", &monitor_window.is_open);
            ImGui::MenuItem("Audio", NULL, &apu_window.m_is_open);
            ImGui::MenuItem("Video", NULL, &video_window.m_is_open);
#ifdef CONFIG_RENDERDOC
            if (nv2a_dbg_renderdoc_available()) {
                ImGui::MenuItem("RenderDoc: Capture", NULL, &g_capture_renderdoc_frame);
            }
#endif
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Help"))
        {
            if (ImGui::MenuItem("Help", NULL)) {
                SDL_OpenURL("https://xemu.app/docs/getting-started/");
            }

            ImGui::MenuItem("Report Compatibility...", NULL,
                            &compatibility_reporter_window.is_open);
#if defined(_WIN32)
            ImGui::MenuItem("Check for Updates...", NULL, &update_window.is_open);
#endif

            ImGui::Separator();
            if (ImGui::MenuItem("About")) g_main_menu.ShowAbout();
            ImGui::EndMenu();
        }

        g_main_menu_height = ImGui::GetWindowHeight();
        ImGui::EndMainMenuBar();
    }
}
