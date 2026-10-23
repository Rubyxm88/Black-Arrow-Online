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
#include "guide.hh"
#include "versions.hh"
#include "game-cache.hh"
#include "xbcheck.hh"
#include "shader-cache.hh"
#include "viewport-manager.hh"
#include "notifications.hh"
#include "../xemu-os-utils.h"
#include "../xemu-xbonline.h"
#include "../xemu-xbcheck.h"
#include "../xemu-diag.h"
#include "hw/xbox/nv2a/shader-precache.h"
#include <cmath>
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

// Explains the menu item just drawn when the mouse rests on it
static void MenuItemHelp(const char *help)
{
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort |
                             ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.0f);
        ImGui::TextUnformatted(help);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

// A menu item with a check mark that flips a setting; hovering explains it.
// The menu stays open, so several settings can be flipped in one go.
static void MenuToggle(const char *label, bool *value, const char *help)
{
    ImGui::PushItemFlag(ImGuiItemFlags_AutoClosePopups, false);
    ImGui::MenuItem(label, NULL, value);
    ImGui::PopItemFlag();
    MenuItemHelp(help);
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

static void OpenFreezeReport()
{
    if (!xemu_diag_open_freeze_report()) {
        xemu_queue_notification("No freeze report yet: one is saved by "
                                "itself when the game freezes");
    }
}

// At the top of the popup column while a diagnostics log is recording: a red
// dot, the time so far and a Stop button. It takes no focus, so the game
// keeps its controls.
void DrawRecordingBadge()
{
    if (!xemu_diag_active()) {
        return;
    }

    PopupStackPlace();
    ImGui::SetNextWindowBgAlpha(0.80f);
    if (ImGui::Begin("Recording Diagnostics Log", NULL,
                     ImGuiWindowFlags_NoDecoration |
                     ImGuiWindowFlags_NoFocusOnAppearing |
                     ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_AlwaysAutoResize)) {
        // A red dot that fades in and out once a second
        float size = ImGui::GetTextLineHeight();
        ImGui::AlignTextToFramePadding();
        ImVec2 p = ImGui::GetCursorScreenPos();
        float y = p.y + ImGui::GetFrameHeight() / 2;
        float pulse = 0.6f + 0.4f * cosf((float)ImGui::GetTime() * 6.2832f);
        ImGui::GetWindowDrawList()->AddCircleFilled(
            ImVec2(p.x + size / 2, y), size * 0.32f,
            IM_COL32(230, 40, 40, (int)(255 * pulse)));
        ImGui::Dummy(ImVec2(size, ImGui::GetFrameHeight()));
        ImGui::SameLine();

        int secs = (int)xemu_diag_seconds();
        ImGui::Text("Recording %d:%02d", secs / 60, secs % 60);
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.70f, 0.12f, 0.12f, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                              ImVec4(0.85f, 0.20f, 0.20f, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                              ImVec4(0.55f, 0.08f, 0.08f, 1));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
        if (ImGui::SmallButton("Stop")) {
            StopDiagnosticsLog();
        }
        ImGui::PopStyleColor(4);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip("Stop recording and open the logs folder. "
                              "Press F9 while recording when the game lags.");
        }
        PopupStackTake();
    }
    ImGui::End();
}

// How long the notice stays after a freeze report was saved
#define FREEZE_TOAST_SECONDS 20.0

// In the popup column, under the XB Online and shader notices
void DrawFreezeToast()
{
    double age = xemu_diag_freeze_age();
    if (age < 0 || age > FREEZE_TOAST_SECONDS) {
        return;
    }

    float scale = g_viewport_mgr.m_scale;
    PopupStackPlace();
    ImGui::SetNextWindowBgAlpha(0.88f);
    if (ImGui::Begin("Freeze Report", NULL,
                     ImGuiWindowFlags_NoDecoration |
                     ImGuiWindowFlags_NoFocusOnAppearing |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_AlwaysAutoResize)) {
        // An exclamation mark in an orange circle, drawn: the font has no
        // such glyph
        float size = ImGui::GetTextLineHeight();
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImVec2 c(p.x + size / 2, p.y + size / 2);
        ImDrawList *dl = ImGui::GetWindowDrawList();
        dl->AddCircleFilled(c, size / 2, IM_COL32(230, 130, 30, 255));
        dl->AddLine(ImVec2(c.x, c.y - size * 0.28f),
                    ImVec2(c.x, c.y + size * 0.08f), IM_COL32_WHITE,
                    2.0f * scale);
        dl->AddCircleFilled(ImVec2(c.x, c.y + size * 0.24f), 1.4f * scale,
                            IM_COL32_WHITE);
        ImGui::Dummy(ImVec2(size, size));
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::Text("The game stopped responding");
        ImGui::TextDisabled("A freeze report was saved:");
        ImGui::TextDisabled("Troubleshoot > Open Last Freeze Report");
        ImGui::EndGroup();
        PopupStackTake();
    }
    ImGui::End();
}

// A slider inside a menu; hovering explains it
static void MenuSlider(const char *label, int *value, int min, int max,
                       const char *format, const char *help)
{
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9.0f);
    ImGui::SliderInt(label, value, min, max, format,
                     ImGuiSliderFlags_AlwaysClamp);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.0f);
        ImGui::TextUnformatted(help);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

// One choice of several, as menu items with a dot on the chosen one
static void MenuChoice(const char *const *labels, int count, int *value)
{
    for (int i = 0; i < count; i++) {
        if (ImGui::MenuItem(labels[i], NULL, *value == i)) {
            *value = i;
        }
    }
}

// View > Performance Overlay > Customize. The menu stays open, so
// changes show right away. Show lists only what the chosen style can show.
static void OverlayMenu()
{
    auto &o = g_config.display.overlay;
    ImGui::PushItemFlag(ImGuiItemFlags_AutoClosePopups, false);

    MenuHeader("Look");
    if (ImGui::BeginMenu("Style")) {
        static const char *const labels[] = {
            "Detailed (frame rate, graph, then a line each)",
            "Compact (all on one line)", "Minimal (big frame rate)"
        };
        MenuChoice(labels, 3, &o.style);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Position")) {
        static const char *const labels[] = { "Top left", "Top right",
                                              "Bottom left", "Bottom right" };
        MenuChoice(labels, 4, &o.position);
        ImGui::Separator();
        MenuSlider("Distance from edge", &o.margin, 0, 80, "%d px",
                   "Space between the overlay and the edge of the window.");
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Size")) {
        static const char *const labels[] = { "Small", "Medium", "Large" };
        MenuChoice(labels, 3, &o.size);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Background")) {
        MenuSlider("Opacity", &o.opacity, 0, 100,
                   o.opacity == 0 ? "None" : "%d%%",
                   "How solid the box behind the numbers is. 0 leaves just "
                   "the text. Under 35% the text gets a dark edge so it stays "
                   "readable on bright pictures.");
        MenuHeader("Color");
        static const char *const labels[] = { "Charcoal", "Black", "Navy",
                                              "Slate" };
        MenuChoice(labels, 4, &o.background_color);
        MenuHeader("Shape");
        MenuToggle("Border", &o.border, "A thin light line around the box.");
        MenuToggle("Rounded corners", &o.rounded, "Rounded or square "
                   "corners.");
        ImGui::EndMenu();
    }

    MenuHeader("Show");
    bool minimal = o.style == CONFIG_DISPLAY_OVERLAY_STYLE_MINIMAL;
    bool detailed = o.style == CONFIG_DISPLAY_OVERLAY_STYLE_DETAILED;
    if (!minimal) {
        MenuToggle("FPS", &o.show_fps, "Frames the game showed in the last "
                   "second.");
    }
    MenuToggle("1% low", &o.show_lows, "The frame rate of the slowest 1% of "
               "frames in the last 10 seconds: low means stutter, even when "
               "the FPS looks fine.");
    if (minimal) {
        ImGui::PopItemFlag();
        return;
    }
    MenuToggle("Frame time", &o.show_frame_time, "Average time per frame in "
               "milliseconds (33.3 ms is 30 FPS, 16.7 ms is 60 FPS).");
    if (detailed) {
        MenuToggle("Frame time graph", &o.show_graph, "The last frames as "
                   "bars: green is on time, yellow, orange and red are "
                   "hitches. The line is the frame time the game aims for.");
    }
    MenuToggle("CPU", &o.show_cpu, "How busy the emulator keeps the "
               "processor, and the whole PC in brackets.");
    MenuToggle("GPU", &o.show_gpu, "How busy the graphics card is (all "
               "programs).");
    MenuToggle("RAM", &o.show_ram, "Memory the emulator uses.");
    MenuToggle("VRAM", &o.show_vram, "Video memory the emulator uses.");
    MenuToggle("Shader building", &o.show_shaders, "Progress while shaders "
               "build in the background.");

    ImGui::PopItemFlag();
}

// Machine: the console itself (pause, snapshots, settings, power)
static void MachineMenu(bool running)
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

    if (ImGui::MenuItem("Settings...")) g_main_menu.ShowSettings();

    ImGui::Separator();

    if (ImGui::MenuItem("Reset", SHORTCUT_MENU_TEXT(R))) ActionReset();
    if (ImGui::MenuItem("Exit", SHORTCUT_MENU_TEXT(Q))) ActionShutdown();
}

// Play: everything that changes the disc in the console
static void PlayMenu()
{
    std::multimap<std::string, std::string> games;
    std::filesystem::path games_dir(g_config.general.games_dir);
    std::error_code ec;
    if (g_config.general.games_dir[0] &&
        std::filesystem::is_directory(games_dir, ec)) {
        for (const auto &file :
             std::filesystem::directory_iterator(games_dir, ec)) {
            const auto &path = file.path();
            if (path.extension() == ".iso" || path.extension() == ".xiso") {
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
    if (games.empty() && ImGui::MenuItem("No games yet: Open Games Folder")) {
        xemu_xbcheck_open_games_folder();
    }
    if (ImGui::MenuItem("Load Disc...", SHORTCUT_MENU_TEXT(O))) ActionLoadDisc();
    if (ImGui::MenuItem("Eject Disc", SHORTCUT_MENU_TEXT(E))) ActionEjectDisc();

    bool ba_updates = qemu_access(XEMU_XBONLINE_UPDATE_DISC, F_OK) == 0;
    bool rs3_updates = qemu_access(XEMU_XBONLINE_RS3_UPDATE_DISC, F_OK) == 0;
    if (ba_updates || rs3_updates) {
        MenuHeader("Game Updates");
    }
    if (ba_updates) {
        if (ImGui::MenuItem("Black Arrow - Install Game Updates")) {
            ActionPlayDiscFile(XEMU_XBONLINE_UPDATE_DISC);
        }
        MenuItemHelp("Installs the Black Arrow title update and map packs on "
                     "this console. Then play Black Arrow from Games.");
    }
    if (rs3_updates) {
        if (ImGui::MenuItem("Old Bow - Install Game Updates")) {
            ActionPlayDiscFile(XEMU_XBONLINE_RS3_UPDATE_DISC);
        }
        MenuItemHelp("Installs the Rainbow Six 3 maps and title update. "
                     "Install only the update that fits your disc (XB Online "
                     "> Connection Check says which, with Rainbow Six 3 "
                     "loaded), and skip Old Scharins: it breaks multiplayer "
                     "with Scharins v2.");
    }

    MenuHeader("Dashboards");
    if (ImGui::MenuItem("XBL Dashboard")) ActionXboxLiveSetup();
    if (ImGui::MenuItem("Original Xbox Dashboard")) ActionXboxDashboard();
}

// XB Online: the Xbox Live account and the connection to XB Online
static void XbOnlineMenu(bool just_opened)
{
    // Read the accounts once each time the menu opens
    static char account_names[128];
    static int account_count = -1;
    if (just_opened) {
        account_count = xemu_xbonline_hdd_account_names(
            g_config.sys.files.hdd_path, account_names,
            sizeof(account_names));
    }

    MenuHeader("Account");
    if (account_count > 0) {
        std::string item =
            std::string(account_count > 1 ? "Accounts: " : "Account: ") +
            account_names;
        if (ImGui::MenuItem(item.c_str())) {
            SDL_OpenURL(XEMU_XBONLINE_URL_DASHBOARD);
        }
        MenuItemHelp("Opens your account page on xb.live.");
    } else if (ImGui::MenuItem("No account yet: add it in XBL Dashboard")) {
        ActionXboxLiveSetup();
    }
    if (ImGui::MenuItem("Create an Account")) {
        SDL_OpenURL(XEMU_XBONLINE_URL_CREATE);
    }
    if (ImGui::MenuItem("Link Code")) {
        SDL_OpenURL(XEMU_XBONLINE_URL_LINK);
    }

    MenuHeader("Connection");
    if (ImGui::MenuItem("Connection Check")) {
        xbcheck_window.Start(true);
    }
    MenuItemHelp("Checks the network, XB Online, the PC clock, the game disc, "
                 "its update and the Xbox Live account.");
}

static void SoundMenu()
{
    MenuHeader("Quality");
    ImGui::PushItemFlag(ImGuiItemFlags_AutoClosePopups, false);
    bool dolby = mcpx_apu_dolby_eeprom();
    if (ImGui::MenuItem("Fast Enhanced (recommended)", NULL,
                        !g_config.audio.use_dsp || dolby)) {
        g_config.audio.use_dsp = false;
    }
    MenuItemHelp("Mixes the game's sounds straight to your speakers. Lightest "
                 "on the PC, steadiest frame rate.");
    if (ImGui::MenuItem("Accurate", NULL, g_config.audio.use_dsp && !dolby,
                        !dolby)) {
        g_config.audio.use_dsp = true;
        g_config.audio.use_dsp_jit = true;
    }
    MenuItemHelp(dolby ? "Off while the Xbox's audio setting is Dolby "
                         "Surround, Dolby Digital or DTS. Set it to Stereo in "
                         "the Xbox Dashboard to use Accurate."
                       : "Runs the Xbox's own sound effects processor, like a "
                         "real console. Uses about a quarter of a CPU core "
                         "more.");
    ImGui::PopItemFlag();

    MenuHeader("Channels");
    ImGui::PushItemFlag(ImGuiItemFlags_AutoClosePopups, false);
    if (ImGui::MenuItem("Stereo", NULL,
                        g_config.audio.channels ==
                            CONFIG_AUDIO_CHANNELS_STEREO)) {
        g_config.audio.channels = CONFIG_AUDIO_CHANNELS_STEREO;
    }
    MenuItemHelp("Two speakers or headphones.");
    if (ImGui::MenuItem("5.1 Surround (beta)", NULL,
                        g_config.audio.channels ==
                            CONFIG_AUDIO_CHANNELS_SURROUND)) {
        g_config.audio.channels = CONFIG_AUDIO_CHANNELS_SURROUND;
    }
    MenuItemHelp("Front, center, sub and rear speakers, from the game's own "
                 "speaker mix. Works with Fast Enhanced sound; Accurate stays "
                 "on the front pair.");
    ImGui::PopItemFlag();

    MenuHeader("Gameplay");
    MenuToggle("Louder movement", &g_config.audio.louder_movement,
               "Makes footsteps and the rattle of gear and clothes easier to "
               "hear nearby.");
}

static void GraphicsMenu()
{
    ShaderPrecacheStatus shader_status;
    nv2a_shader_precache_get_status(&shader_status);
    bool opengl_running = shader_status.supported;

    MenuHeader("Quality");
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
    MenuToggle("Enhanced graphics", &g_config.display.quality.enhanced,
               "Sharpens textures, distant floors and menu text, and smooths "
               "jagged edges.");

    MenuHeader("Display");
    if (ImGui::MenuItem("Fullscreen", "F11", xemu_is_fullscreen(), true)) {
        xemu_toggle_fullscreen();
    }
    MenuToggle("Vertical sync", &g_config.display.window.vsync,
               "Stops screen tearing. Turning it off can lower input delay "
               "but may tear.");
    if (ImGui::BeginMenu("Window & Scaling")) {
        ImGui::PushItemFlag(ImGuiItemFlags_AutoClosePopups, false);
        if (ImGui::BeginMenu("Aspect Ratio")) {
            static const char *const ratios[] = {
                "Native", "Auto", "4:3", "16:9"
            };
            for (int i = 0; i < 4; i++) {
                if (ImGui::MenuItem(ratios[i], NULL,
                                    g_config.display.ui.aspect_ratio == i)) {
                    g_config.display.ui.aspect_ratio = i;
                }
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Display Mode")) {
            static const char *const fits[] = {
                "Center", "Scale", "Stretch"
            };
            for (int i = 0; i < 3; i++) {
                if (ImGui::MenuItem(fits[i], NULL,
                                    g_config.display.ui.fit == i)) {
                    g_config.display.ui.fit = i;
                }
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("UI Scale")) {
            if (ImGui::MenuItem("Auto", NULL, g_config.display.ui.auto_scale)) {
                g_config.display.ui.auto_scale = true;
            }
            if (ImGui::MenuItem("1x", NULL,
                                !g_config.display.ui.auto_scale &&
                                    g_config.display.ui.scale == 1)) {
                g_config.display.ui.auto_scale = false;
                g_config.display.ui.scale = 1;
            }
            if (ImGui::MenuItem("2x", NULL,
                                !g_config.display.ui.auto_scale &&
                                    g_config.display.ui.scale == 2)) {
                g_config.display.ui.auto_scale = false;
                g_config.display.ui.scale = 2;
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Filter Method")) {
            if (ImGui::MenuItem("Linear", NULL,
                                g_config.display.filtering == 0)) {
                g_config.display.filtering = 0;
            }
            if (ImGui::MenuItem("Nearest", NULL,
                                g_config.display.filtering == 1)) {
                g_config.display.filtering = 1;
            }
            ImGui::EndMenu();
        }
        ImGui::PopItemFlag();
        ImGui::EndMenu();
    }

    MenuHeader("Shaders");
    MenuToggle("Save shaders to disk", &g_config.perf.cache_shaders,
               "Keep every shader the game builds, so it never has to build "
               "it again on this PC.");
    ImGui::BeginDisabled(!opengl_running);
    MenuToggle("Build shaders in the background", &g_config.perf.shader_async,
               "Instead of freezing while a new shader builds, skip that "
               "effect for a few frames. Turn off if effects flicker. OpenGL "
               "only.");
    ImGui::EndDisabled();
    if (ImGui::BeginMenu("Precache")) {
        MenuToggle("Precache shaders automatically",
                   &g_config.perf.shader_precache_auto,
                   "Build missing shaders in the background by themselves at "
                   "start (first run, after a graphics driver update or a "
                   "resolution change), with a small notice. Turn off to only "
                   "precache from Troubleshoot > Shader Cache...");
        MenuToggle("Precache this PC's own shaders",
                   &g_config.perf.shader_precache_own,
                   "Precache Shaders also rebuilds every shader this PC has "
                   "used before, not only the Black Arrow list.");
        ImGui::EndMenu();
    }

    MenuHeader("Performance");
    MenuToggle("Improve render handling",
               &g_config.display.quality.skip_reuse_copyback,
               "Improves frame rate and reduces stutter when rendering screen "
               "effects. Turn off if effects look wrong. OpenGL only.");
    if (ImGui::BeginMenu("Performance Overlay")) {
        MenuToggle("Show performance overlay",
                   &g_config.display.overlay.enabled,
                   "Frame rate, 1% lows, frame times and how busy the PC is, "
                   "over the game. The frame times are the game's own frames, "
                   "so every hitch shows.");
        ImGui::Separator();
        OverlayMenu();
        ImGui::EndMenu();
    }
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
    if (opengl_running !=
        (g_config.display.renderer == CONFIG_DISPLAY_RENDERER_OPENGL)) {
        ImGui::TextDisabled("  Restart Black Arrow Online to switch renderer");
    }
}

// Troubleshoot: what to use when something goes wrong
static void TroubleshootMenu()
{
    MenuHeader("Connection");
    if (ImGui::MenuItem("Connection Check")) {
        xbcheck_window.Start(true);
    }
    MenuItemHelp("Checks the network, XB Online, the PC clock, the game disc, "
                 "its update and the Xbox Live account.");

    MenuHeader("Lag & Freezes");
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
    MenuItemHelp("Records a line for every second while you play, to find out "
                 "what causes lag. Press F9 when it lags.");
    MenuToggle("Save a report when the game freezes",
               &g_config.perf.freeze_report,
               "When the game shows no new picture for 10 seconds, save a "
               "short report of the last minute and what the Xbox was doing "
               "to the logs folder. Nothing is written until then.");
    if (ImGui::MenuItem("Open Last Freeze Report")) {
        OpenFreezeReport();
    }

    MenuHeader("Caches");
    if (ImGui::MenuItem("Shader Cache...")) {
        shader_cache_window.Open();
    }
    MenuItemHelp("What the shader cache holds; precache or clear it.");
    if (ImGui::MenuItem("Empty Game Cache...")) {
        game_cache_window.Open();
    }
    MenuItemHelp("The game keeps copies of disc files on the console's hard "
                 "disk. They are emptied by themselves when you play a "
                 "different disc of the same game; this empties them now.");

    MenuHeader("Files");
    if (ImGui::MenuItem("Open Logs Folder")) {
        xemu_diag_open_folder();
    }

    if (ImGui::BeginMenu("Developer")) {
        ImGui::MenuItem("Monitor", "~", &monitor_window.is_open);
        ImGui::MenuItem("Audio", NULL, &apu_window.m_is_open);
        ImGui::MenuItem("Video", NULL, &video_window.m_is_open);
#ifdef CONFIG_RENDERDOC
        if (nv2a_dbg_renderdoc_available()) {
            ImGui::MenuItem("RenderDoc: Capture", NULL,
                            &g_capture_renderdoc_frame);
        }
#endif
        ImGui::EndMenu();
    }
}

static void HelpMenu()
{
    if (ImGui::MenuItem("Black Arrow Online Guide...")) {
        guide_window.Open();
    }
    if (ImGui::MenuItem("Versions & Updates...")) {
        versions_window.Open();
    }
    ImGui::Separator();
    if (ImGui::MenuItem("About")) g_main_menu.ShowAbout();
}

void ShowMainMenu()
{
    bool running = runstate_is_running();

    if (ImGui::BeginMainMenuBar())
    {
        if (ImGui::BeginMenu("Machine")) {
            MachineMenu(running);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Play")) {
            PlayMenu();
            ImGui::EndMenu();
        }
        static bool xbonline_menu_open;
        if (ImGui::BeginMenu("XB Live")) {
            XbOnlineMenu(!xbonline_menu_open);
            xbonline_menu_open = true;
            ImGui::EndMenu();
        } else {
            xbonline_menu_open = false;
        }
        if (ImGui::BeginMenu("Sound")) {
            SoundMenu();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Graphics")) {
            GraphicsMenu();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Troubleshoot")) {
            TroubleshootMenu();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Help")) {
            HelpMenu();
            ImGui::EndMenu();
        }

        DrawBuildBadge();

        g_main_menu_height = ImGui::GetWindowHeight();
        ImGui::EndMainMenuBar();
    }
}
