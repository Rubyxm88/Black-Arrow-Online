//
// xemu User Interface: Game Cache window
//
// Empties the copy of disc files the Xbox keeps on the hard disk for the game
// in the drive, and restarts the console so the game copies them again. This
// already happens by itself when a changed disc image is loaded (see
// ui/xemu-gamecache.h); the window is for when that was not enough.
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#include "common.hh"
#include "game-cache.hh"
#include "actions.hh"
#include "../xemu-notifications.h"
#include "viewport-manager.hh"
#include "../xemu-gamecache.h"
#include "../xemu-xbonline.h"

GameCacheWindow game_cache_window;

GameCacheWindow::GameCacheWindow()
{
    is_open = false;
    m_has_disc = false;
    m_title_id = 0;
    m_name[0] = 0;
    m_has_cache = -1;
    m_result = 2;
}

void GameCacheWindow::Open()
{
    m_has_disc = xemu_gamecache_disc_title(&m_title_id, m_name, sizeof(m_name));
    if (!m_has_disc) {
        // No game in the drive: Black Arrow is the one that matters
        m_title_id = XEMU_XBONLINE_TITLE_ID;
        snprintf(m_name, sizeof(m_name), "Black Arrow");
    }
    m_has_cache = xemu_gamecache_has(m_title_id);
    m_result = 2;
    is_open = true;
}

void GameCacheWindow::Draw()
{
    if (!is_open) {
        return;
    }

    float scale = g_viewport_mgr.m_scale;
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(520.0f * scale, 0), ImGuiCond_Appearing);
    if (!ImGui::Begin("Empty Game Cache", &is_open,
                      ImGuiWindowFlags_NoCollapse |
                      ImGuiWindowFlags_NoSavedSettings |
                      ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }

    ImGui::TextWrapped(
        "The Xbox keeps a copy of files a game reads from its disc on the "
        "hard disk. When you load a changed disc image (a new version or a "
        "mod), this copy is emptied by itself, so you do not normally need "
        "this. Emptying it restarts the console; the next start of the game "
        "takes a little longer while it copies the files again. Saves, "
        "accounts and game updates are not touched.");
    ImGui::Separator();

    ImGui::Text("Game: %s%s", m_name, m_has_disc ? "" : " (no game disc in the "
                                                       "drive)");
    if (m_result == 1) {
        ImGui::TextColored(ImVec4(0.45f, 0.9f, 0.45f, 1.0f),
                           "Emptied. The console is restarting.");
    } else if (m_result == -1 || m_has_cache < 0) {
        ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.35f, 1.0f),
                           "The hard disk could not be read or written.");
    } else if (m_has_cache == 0) {
        ImGui::TextDisabled("This game has no copy on the hard disk.");
    } else {
        ImGui::Text("This game has a copy on the hard disk.");
    }

    ImGui::Separator();
    ImGui::BeginDisabled(m_has_cache != 1 || m_result == 1);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.13f, 0.12f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                          ImVec4(0.70f, 0.18f, 0.16f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                          ImVec4(0.45f, 0.10f, 0.09f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
    if (ImGui::Button("Empty and Restart")) {
        m_result = xemu_gamecache_empty(m_title_id);
        if (m_result == 1) {
            m_has_cache = 0;
            ActionReset();
            xemu_queue_notification("Game cache emptied, the console "
                                    "restarted");
        }
    }
    ImGui::PopStyleColor(4);
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Close")) {
        is_open = false;
    }

    ImGui::End();
}
