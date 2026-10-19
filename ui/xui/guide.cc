//
// xemu User Interface: Black Arrow Online guide (Help > Black Arrow Online
// Guide...)
//
// The package README, inside the app: topics on the left, the chosen one on
// the right. A topic can end with buttons that do what it describes, so a
// player does not have to find the menu item.
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#include "common.hh"
#include "guide.hh"
#include "actions.hh"
#include "main-menu.hh"
#include "versions.hh"
#include "game-cache.hh"
#include "xbcheck.hh"
#include "shader-cache.hh"
#include "viewport-manager.hh"
#include "font-manager.hh"
#include "../xemu-xbonline.h"
#include "../xemu-xbcheck.h"
#include "../xemu-diag.h"

#include <SDL3/SDL.h>

GuideWindow guide_window;

static const ImVec4 heading_color(0.42f, 0.80f, 0.30f, 1.0f);
static const ImVec4 xbox_green(0.063f, 0.486f, 0.063f, 1.0f);
static const ImVec4 xbox_green_hover(0.10f, 0.62f, 0.10f, 1.0f);
static const ImVec4 xbox_green_active(0.05f, 0.40f, 0.05f, 1.0f);

struct GuideAction {
    const char *label;
    void (*run)(void);
};

struct GuidePage {
    const char *title;
    // Paragraphs; a line starting with "# " is a heading, "- " a bullet,
    // "> " a menu path
    const char *const *text;
    GuideAction actions[3];
};

static void OpenGamesFolder() { xemu_xbcheck_open_games_folder(); }
static void OpenXblDashboard() { ActionXboxLiveSetup(); }
static void OpenLinkCode() { SDL_OpenURL(XEMU_XBONLINE_URL_LINK); }
static void OpenCreateAccount() { SDL_OpenURL(XEMU_XBONLINE_URL_CREATE); }
static void RunConnectionCheck() { xbcheck_window.Start(true); }
static void InstallBaUpdates() { ActionPlayDiscFile(XEMU_XBONLINE_UPDATE_DISC); }
static void OpenShaderCache() { shader_cache_window.Open(); }
static void OpenGameCache() { game_cache_window.Open(); }
static void OpenLogs() { xemu_diag_open_folder(); }
static void OpenVersions() { versions_window.Open(); }
static void OpenSettings() { g_main_menu.ShowSettings(); }

static const char *const start_text[] = {
    "# First time",
#if defined(__APPLE__)
    "- Install with the Terminal line from README.txt (setup.sh). It puts "
    "the app in Applications and sets up your console.",
    "- Drag your Black Arrow disc image (.iso) into the games folder that "
    "opens (Open Games Folder below).",
    "- Open Black Arrow Online from Applications. When macOS asks whether it "
    "may accept incoming network connections or find devices on your local "
    "network, click Allow: XB Online needs both.",
#else
    "- Copy your Black Arrow disc image (.iso) into the \"games\" folder.",
    "- Start Black Arrow Online. A setup window appears once: Start clean "
    "for a brand new console, or Use my existing xemu files to bring your own "
    "hard disk (xbox_hdd.qcow2) and console identity (eeprom.bin).",
#endif
    "- The XBL Dashboard opens. Sign in with your xb.live account. No account "
    "yet? See Xbox Live Account first.",
    "- In the dashboard: Settings > Network, set the top option to XB Online.",
    "- Install the game updates (see Game Updates), then play:",
    "> Play > Play Rainbow Six 3 Black Arrow",
    "# Every time after that",
    "Start Black Arrow Online and it goes straight to the game. With no game "
    "in the games folder it opens the XBL Dashboard instead.",
    NULL
};

static const char *const menus_text[] = {
    "# Machine",
    "Pause, screenshots, snapshots, Settings... (controllers, audio device, "
    "network), Reset and Exit.",
    "# Play",
    "Every game in the games folder, Load Disc and Eject Disc, the game "
    "update installers and the two dashboards.",
    "# XB Live",
    "Your Xbox Live account (opens xb.live), Create an Account, Link Code and "
    "the Connection Check.",
    "# Sound",
    "Sound quality (Fast or Accurate) and Louder movement.",
    "# Graphics",
    "Resolution, Enhanced graphics, fullscreen (F11), window scaling, "
    "shaders and the performance overlay.",
    "# Troubleshoot",
    "Everything for when something goes wrong: Connection Check, diagnostics "
    "log, freeze reports, the shader and game caches, the logs folder.",
    "# Help",
    "This guide, Versions & Updates and About. Click the build number on the "
    "right of the menu bar for Versions & Updates too.",
    "On a controller, press the Guide/Home button for the same options.",
#if defined(__APPLE__)
    "On a Mac laptop keyboard, F9 and the other F keys may need the fn key.",
#endif
    NULL
};

static const char *const account_text[] = {
    "Xbox Live accounts for XB Online are made on xb.live.",
    "- Create the account on xb.live/xb-online:",
    "> XB Live > Create an Account",
    "- Sign in to the XBL Dashboard and link the account. Link Code gives "
    "you the code:",
    "> XB Live > Link Code",
    "Your gamertag then shows at the top of the XB Live menu. In the game, "
    "sign in with the account the XBL Dashboard added.",
    NULL
};

static const char *const updates_text[] = {
    "# Black Arrow",
    "The title update and map packs go on the console's hard disk once:",
    "> Play > Black Arrow - Install Game Updates",
    "Follow the prompts. Skip this if your own hard disk already has them.",
    "# Rainbow Six 3 (Old Bow)",
    "Rainbow Six 3 has its own installer, Play > Old Bow - Install Game "
    "Updates. Its title update must fit your disc, or the game will not "
    "start:",
    "- Disc version 1 (all regions): Game Update Menu > Install Update RF",
    "- Disc version 2 (PAL German): Game Update Menu > Install Update Ger",
    "- Disc version 3 or 4: no update, it is already on the disc",
    "With the Rainbow Six 3 disc loaded, the Connection Check says which. "
    "Skip \"Old Scharins\": it breaks multiplayer with Scharins v2.",
    NULL
};

static const char *const settings_text[] = {
    "# Graphics",
    "- Resolution: 1x (fastest) to 4x. 2x is the default.",
    "- Enhanced graphics: sharpens textures, distant floors and menu text, "
    "and smooths jagged edges.",
#if defined(__APPLE__)
    "- Renderer: always OpenGL on a Mac.",
#else
    "- Renderer: OpenGL (recommended) or Vulkan. Needs a restart. Shader "
    "caching works with OpenGL only.",
#endif
    "- Improve render handling: speeds up screen effects. Turn it off if "
    "effects look wrong.",
    "# Sound",
    "- Fast (recommended): mixes the game's sounds straight to your speakers.",
    "- Accurate: runs the Xbox's own sound effects processor, about a quarter "
    "of a CPU core more.",
    "- Louder movement: makes footsteps and the rattle of gear and clothes "
    "easier to hear nearby.",
    "# Shaders",
    "The first time an effect appears (a map, night vision, a menu) your "
    "graphics card builds a shader for it, which used to make the game hitch. "
    "Black Arrow Online ships a list of the game's shaders and builds the "
    "missing ones in the background a few seconds after starting. It happens "
    "again only after a graphics driver update or a resolution change.",
    "# Performance overlay",
    "Graphics > Performance Overlay > Show performance overlay: frame rate, "
    "1% lows, frame times, CPU, GPU and memory over the game.",
    NULL
};

static const char *const trouble_text[] = {
    "# \"Xbox Live not found\" or sign-in fails",
    "- Run the Connection Check and fix whatever it marks FIX or WARN first.",
    "- VPN: turn it off completely (also auto-connect and kill switch) and "
    "restart. A sign: the dashboard's Settings > Network shows an address "
    "that is not this PC's (for example 10.14.0.2).",
#if defined(__APPLE__)
    "- Clock: sign-in fails when the Mac's clock or time zone is wrong. Set "
    "both automatically in System Settings > General > Date & Time.",
    "- Local network: System Settings > Privacy & Security > Local Network "
    "must allow Black Arrow Online.",
#else
    "- PC clock: sign-in fails when the Windows clock or time zone is wrong. "
    "Set both automatically in Windows Settings > Time.",
#endif
    "- A firewall, antivirus or router DNS setting can be in the way: allow "
    "xemu through the firewall, or try another network.",
    "- Close any other emulator or game, then start again.",
#if defined(__APPLE__)
    "- Still failing: double-click Black Arrow Report in Applications and "
    "send the zip it makes in Downloads to whoever shared the package.",
#else
    "- Still failing: send system\\xemu.log to whoever shared the package.",
#endif
    "# Game looks wrong",
    "Graphics: turn off Improve render handling. Effects missing or "
    "flickering: turn off Build shaders in the background in Graphics.",
    "# Slow or stuttering",
#if defined(__APPLE__)
    "Graphics > Resolution: 1x.",
#else
    "Graphics > Resolution: 1x. On a 144 Hz or 165 Hz monitor, set it to "
    "120 Hz or 60 Hz: the game runs at 30 frames.",
#endif
    "# Lag",
    "Troubleshoot > Record Diagnostics Log, play, and press F9 right after "
    "each lag spike. Stop it from the same menu. Send the log (logs folder) "
    "to whoever shared the package: it holds your PC's specs and frame "
    "times, not your account.",
    "# Freezes",
    "When the game shows no new picture for 10 seconds, a freeze report is "
    "saved by itself. Troubleshoot > Open Last Freeze Report.",
    "# A modded disc plays like the original",
    "The game keeps copies of disc files on the console's hard disk. They are "
    "emptied by themselves when you play a different disc of the same game; "
    "Troubleshoot > Empty Game Cache... empties them now.",
    NULL
};

static const char *const hosting_text[] = {
    "- Players on your own network (another Xbox or PC) can join as it is. "
#if defined(__APPLE__)
    "Allow incoming connections when macOS asks.",
#else
    "Allow xemu through Windows Firewall when Windows asks.",
#endif
    "- Players over the internet can join only if your router forwards UDP "
    "port 3074 to this PC.",
#if defined(__APPLE__)
    "- Only one copy of Black Arrow Online per Mac can host.",
#else
    "- Only one copy of Black Arrow Online per PC can host.",
#endif
    NULL
};

static const char *const versions_text[] = {
#if defined(__APPLE__)
    "When a newer build is out, a small panel in the top left says so a few "
    "seconds after the app starts. Update now downloads it, closes the app "
    "and opens it again on the new build within about a minute.",
#else
    "When a newer build is out, Black Arrow Online offers it when you start.",
#endif
    "Help > Versions & Updates lists every build: install the newest, or go "
    "back to an earlier one if a new build gives you trouble. It asks first, "
    "then closes Black Arrow Online, installs the build and opens it again.",
#if defined(__APPLE__)
    "An update replaces the app only. Your console, Xbox Live sign-in, saves, "
    "settings and games are kept.",
#else
    "An update replaces the launcher, the emulator and the shader list only. "
    "Your console, Xbox Live sign-in, saves, settings and games are kept.",
#endif
    "Builds marked Broken can not be installed: they had a problem that was "
    "found after they came out.",
#if defined(__APPLE__)
    "Build 14 does not have this window. To come back from it, choose Update "
    "now when the app offers the newest build after starting.",
    "If an update cannot be installed, the app says why the next time it "
    "opens. To update by hand, download BlackArrowOnline-mac-update.zip from "
    "the releases page and run the Terminal line from README.txt.",
#else
    "Builds before 15 do not have this window. To come back from one, choose "
    "Update now when the launcher offers the newest build at the start.",
    "To turn the check at start off, create an empty file "
    "system\\no-update-check.txt.",
#endif
    NULL
};

static const char *const files_text[] = {
#if defined(__APPLE__)
    "# Where things are",
    "- The app: Applications > Black Arrow Online (or Applications in your "
    "home folder, if setup said so).",
    "- Your console: ~/Library/Application Support/Black Arrow Online/system",
    "- Your games: ~/Library/Application Support/Black Arrow Online/games",
    "To open one in Finder: Go > Go to Folder..., paste the path, press "
    "Return.",
    "# Starting over",
    "Run the Terminal line from README.txt with --start-over at the end. Your "
    "current console, with its Xbox Live account and saves, is lost.",
    "# Do not share",
    "system/eeprom.bin and system/xbox_hdd.qcow2 belong to your console and "
    "your account. Never send them to anyone. Share the original zip instead.",
    "# Controller",
    "Pair it in Bluetooth settings before starting and it is used by itself. "
    "To change it: Machine > Settings... > Input.",
#else
    "# Starting over",
    "\"system\\Start over.bat\" removes the console in this folder. The next "
    "start asks again whether you want a clean start or your own files.",
    "# Do not share",
    "After setup, system\\eeprom.bin and system\\xbox_hdd.qcow2 belong to your "
    "console and your account. Never send them to anyone. Share the original "
    "zip instead.",
    "# Controller",
    "Plug a controller in before starting and it is used by itself. To change "
    "it: Machine > Settings... > Input.",
#endif
    NULL
};

static const GuidePage pages[] = {
    { "Getting Started", start_text,
      { { "Open Games Folder", OpenGamesFolder },
        { "Open XBL Dashboard", OpenXblDashboard } } },
    { "The Menus", menus_text, {} },
    { "Xbox Live Account", account_text,
      { { "Create an Account", OpenCreateAccount },
        { "Link Code", OpenLinkCode } } },
    { "Game Updates", updates_text,
      { { "Install Black Arrow Updates", InstallBaUpdates } } },
    { "Graphics & Sound", settings_text,
      { { "Shader Cache...", OpenShaderCache } } },
    { "Troubleshooting", trouble_text,
      { { "Connection Check", RunConnectionCheck },
        { "Empty Game Cache...", OpenGameCache },
        { "Open Logs Folder", OpenLogs } } },
    { "Hosting a Game", hosting_text, {} },
    { "Versions & Updates", versions_text,
      { { "Versions & Updates...", OpenVersions } } },
    { "Your Files", files_text,
      { { "Machine Settings...", OpenSettings } } },
};

GuideWindow::GuideWindow()
{
    is_open = false;
    m_page = 0;
}

void GuideWindow::Open(int page)
{
    is_open = true;
    m_page = page;
    ImGui::SetWindowFocus("Black Arrow Online Guide");
}

static void DrawText(const char *const *text)
{
    float scale = g_viewport_mgr.m_scale;
    for (int i = 0; text[i]; i++) {
        const char *line = text[i];
        if (!strncmp(line, "# ", 2)) {
            if (i > 0) {
                ImGui::Dummy(ImVec2(0, 4 * scale));
            }
            ImGui::PushStyleColor(ImGuiCol_Text, heading_color);
            ImGui::TextUnformatted(line + 2);
            ImGui::PopStyleColor();
        } else if (!strncmp(line, "- ", 2)) {
            ImGui::Bullet();
            ImGui::TextWrapped("%s", line + 2);
        } else if (!strncmp(line, "> ", 2)) {
            // A menu path, set off like a key the player presses
            ImGui::Indent(14 * scale);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.92f, 0.80f, 1));
            ImGui::TextWrapped("%s", line + 2);
            ImGui::PopStyleColor();
            ImGui::Unindent(14 * scale);
        } else {
            ImGui::TextWrapped("%s", line);
        }
        ImGui::Dummy(ImVec2(0, 2 * scale));
    }
}

void GuideWindow::Draw()
{
    if (!is_open) {
        return;
    }

    float scale = g_viewport_mgr.m_scale;
    ImGuiIO &io = ImGui::GetIO();
    ImGui::SetNextWindowSize(ImVec2(720 * scale, 470 * scale),
                             ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(
        ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
        ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::Begin("Black Arrow Online Guide", &is_open,
                      ImGuiWindowFlags_NoCollapse |
                      ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::End();
        return;
    }

    int count = (int)(sizeof(pages) / sizeof(pages[0]));
    if (m_page < 0 || m_page >= count) {
        m_page = 0;
    }

    ImGui::BeginChild("topics", ImVec2(190 * scale, 0), true);
    for (int i = 0; i < count; i++) {
        if (ImGui::Selectable(pages[i].title, m_page == i)) {
            m_page = i;
        }
    }
    ImGui::EndChild();
    ImGui::SameLine();

    const GuidePage &page = pages[m_page];
    float footer = page.actions[0].label ? ImGui::GetFrameHeightWithSpacing() +
                                               ImGui::GetStyle().ItemSpacing.y
                                         : 0;
    ImGui::BeginGroup();
    ImGui::BeginChild("page", ImVec2(0, -footer));
    ImGui::PushFont(g_font_mgr.m_menu_font_medium);
    ImGui::TextUnformatted(page.title);
    ImGui::PopFont();
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, 2 * scale));
    DrawText(page.text);
    ImGui::EndChild();

    if (page.actions[0].label) {
        ImGui::PushStyleColor(ImGuiCol_Button, xbox_green);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, xbox_green_hover);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, xbox_green_active);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
        for (int i = 0; i < 3 && page.actions[i].label; i++) {
            if (i > 0) {
                ImGui::SameLine();
            }
            if (ImGui::Button(page.actions[i].label)) {
                page.actions[i].run();
            }
        }
        ImGui::PopStyleColor(4);
    }
    ImGui::EndGroup();

    ImGui::End();
}
