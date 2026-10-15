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
#include "../xemu-os-utils.h"
#include "../xemu-xbonline.h"
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

        if (ImGui::BeginMenu("XB Online"))
        {
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

            std::filesystem::path current(g_config.sys.files.dvd_path);
            for (const auto &[label, path] : games) {
                std::string item = "Play " + label;
                bool loaded = std::filesystem::equivalent(current, path, ec);
                if (ImGui::MenuItem(item.c_str(), NULL, loaded)) {
                    ActionPlayDiscFile(path.c_str());
                }
            }
            if (games.empty()) {
                ImGui::MenuItem("No games in the games folder", NULL, false,
                                false);
            }
            if (ImGui::MenuItem("Add or Load Another Game...")) ActionLoadDisc();

            ImGui::Separator();

            if (ImGui::MenuItem("XBL Dashboard (Account Setup)")) ActionXboxLiveSetup();
            if (ImGui::MenuItem("Original Xbox Dashboard")) ActionXboxDashboard();
            if (qemu_access(XEMU_XBONLINE_UPDATE_DISC, F_OK) == 0 &&
                ImGui::MenuItem("Install Game Update + Maps")) {
                ActionPlayDiscFile(XEMU_XBONLINE_UPDATE_DISC);
            }
            if (ImGui::MenuItem("Get a Link Code (xb.live)")) {
                SDL_OpenURL("https://xb.live/link");
            }

            if (qemu_access(XEMU_XBONLINE_INSIGNIA_DISC, F_OK) == 0) {
                ImGui::Separator();
                if (ImGui::MenuItem("New Account: Register Xbox (Insignia)")) {
                    ActionPlayDiscFile(XEMU_XBONLINE_INSIGNIA_DISC);
                }
                if (ImGui::MenuItem("New Account: Get a Code (insignia.live)")) {
                    SDL_OpenURL("https://insignia.live/");
                }
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
