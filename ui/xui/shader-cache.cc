//
// xemu User Interface: Shader Cache window
//
// Shows what the shader cache holds, builds the shaders of the shipped list
// ahead of time (Precache Shaders) and empties the cache (Clear Cache). The
// first time a list has shaders this PC has not built yet, the window opens
// by itself once to offer the precache.
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#include "common.hh"
#include "shader-cache.hh"
#include "viewport-manager.hh"
#include "../xemu-settings.h"
#include "hw/xbox/nv2a/shader-precache.h"

/* Seconds after starting before looking whether to offer a precache */
#define OFFER_DELAY 12.0

/* Remembers the driver the precache was offered for, in the base folder */
#define OFFER_FILE "shader-precache-offered.txt"

static const ImVec4 xbox_green(0.063f, 0.486f, 0.063f, 1.0f);
static const ImVec4 xbox_green_hover(0.10f, 0.62f, 0.10f, 1.0f);
static const ImVec4 xbox_green_active(0.05f, 0.40f, 0.05f, 1.0f);

static void PushButtonColors(const ImVec4 &base, const ImVec4 &hover,
                             const ImVec4 &active)
{
    ImGui::PushStyleColor(ImGuiCol_Button, base);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, active);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
}

static void PushGreenButtons()
{
    PushButtonColors(xbox_green, xbox_green_hover, xbox_green_active);
}

static void PushRedButtons()
{
    PushButtonColors(ImVec4(0.55f, 0.13f, 0.12f, 1.0f),
                     ImVec4(0.70f, 0.18f, 0.16f, 1.0f),
                     ImVec4(0.45f, 0.10f, 0.09f, 1.0f));
}

static void PopButtonColors()
{
    ImGui::PopStyleColor(4);
}

static char *OfferFilePath()
{
    return g_strdup_printf("%s%s", xemu_settings_get_base_path(), OFFER_FILE);
}

ShaderCacheWindow shader_cache_window;

ShaderCacheWindow::ShaderCacheWindow()
{
    is_open = false;
    m_offer_checked = false;
    m_scan_requested = false;
    m_confirm_clear = false;
    m_cleared_time = -1;
}

void ShaderCacheWindow::Open()
{
    nv2a_shader_precache_scan();
    m_confirm_clear = false;
    is_open = true;
}

// Open once per graphics driver when the list has shaders to build
void ShaderCacheWindow::CheckOffer()
{
    if (m_offer_checked || !g_config.perf.shader_precache_offer ||
        ImGui::GetTime() < OFFER_DELAY) {
        return;
    }

    ShaderPrecacheStatus status;
    nv2a_shader_precache_get_status(&status);
    if (!status.supported) {
        return;
    }
    if (!m_scan_requested) {
        nv2a_shader_precache_scan();
        m_scan_requested = true;
        return;
    }
    if (!status.scanned || status.running) {
        return;
    }
    m_offer_checked = true;
    if (status.list_missing <= 0) {
        return;
    }

    /* Offer again after a driver update or a resolution scale change */
    char *path = OfferFilePath();
    char *want = g_strdup_printf("%s, scale %u", status.driver,
                                 nv2a_get_surface_scale_factor());
    gchar *offered = NULL;
    bool same = g_file_get_contents(path, &offered, NULL, NULL) &&
                !strcmp(offered, want);
    if (!same) {
        g_file_set_contents(path, want, -1, NULL);
        is_open = true;
    }
    g_free(offered);
    g_free(want);
    g_free(path);
}

void ShaderCacheWindow::Draw()
{
    CheckOffer();
    if (!is_open) {
        return;
    }

    ShaderPrecacheStatus status;
    nv2a_shader_precache_get_status(&status);

    float scale = g_viewport_mgr.m_scale;
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(560.0f * scale, 0), ImGuiCond_Appearing);
    if (!ImGui::Begin("Shader Cache", &is_open,
                      ImGuiWindowFlags_NoCollapse |
                      ImGuiWindowFlags_NoSavedSettings |
                      ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }

    ImGui::TextWrapped(
        "The first time the game shows a new effect (gunfire, doors, "
        "explosions, a new area) it builds a shader for your graphics card, "
        "which makes the game stutter for a moment. Precaching builds the "
        "shaders Black Arrow uses ahead of time. They are kept across "
        "updates; after a graphics driver update they are built again.");
    ImGui::Separator();

    if (!status.supported) {
        ImGui::TextWrapped("Not available with Vulkan. Shader precaching "
                           "works with the OpenGL renderer: choose it under "
                           "RSO Settings > Renderer and restart.");
    } else {
        ImGui::Text("Graphics card: %s", status.gpu);
        ImGui::Text("Driver: %s", status.driver);
        if (status.scanned) {
            ImGui::Text("Shaders saved on this PC: %d (%.1f MB)",
                        status.disk_count, status.disk_bytes / 1048576.0);
            if (status.list_total < 0) {
                ImGui::TextDisabled("No shader list in this package.");
            } else if (!status.running) {
                ImGui::Text("Shaders to have ready at %ux: %d, %d still to "
                            "build", nv2a_get_surface_scale_factor(),
                            status.list_total, status.list_missing);
                ImGui::TextDisabled(g_config.perf.shader_precache_own ?
                                    "From the Black Arrow list and the "
                                    "shaders this PC has used." :
                                    "From the Black Arrow list.");
            }
        } else {
            ImGui::TextDisabled("Counting shaders...");
        }
    }

    if (status.building) {
        int total = status.list_missing;
        int finished = status.done + status.failed;
        char label[64];
        snprintf(label, sizeof(label), "%d / %d", finished, total);
        ImGui::ProgressBar(total ? (float)finished / total : 0.0f,
                           ImVec2(-1, 0), label);
        ImGui::TextWrapped("Building in the background. You can keep "
                           "playing; the game may run a little slower until "
                           "it finishes.");
    } else if (status.done || status.failed) {
        ImGui::TextColored(ImVec4(0.45f, 0.9f, 0.45f, 1.0f),
                           "Built %d shaders.", status.done);
        if (status.failed) {
            ImGui::SameLine();
            ImGui::TextDisabled("(%d could not be built)", status.failed);
        }
    }
    if (status.error[0]) {
        ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.35f, 1.0f), "%s",
                           status.error);
    }
    if (m_cleared_time >= 0 && ImGui::GetTime() - m_cleared_time < 8.0) {
        ImGui::TextWrapped("Cache cleared. Shaders already in use stay until "
                           "Black Arrow Online is restarted.");
    }

    ImGui::Separator();
    bool can_build = status.supported && status.scanned && !status.running &&
                     status.list_missing > 0;
    PushGreenButtons();
    if (status.running) {
        if (ImGui::Button("Stop")) {
            nv2a_shader_precache_stop();
        }
    } else {
        ImGui::BeginDisabled(!can_build);
        if (ImGui::Button("Precache Shaders")) {
            m_confirm_clear = false;
            nv2a_shader_precache_start();
        }
        ImGui::EndDisabled();
    }
    PopButtonColors();

    ImGui::SameLine();
    PushRedButtons();
    ImGui::BeginDisabled(!status.supported || status.running);
    if (ImGui::Button(m_confirm_clear ? "Click again to clear"
                                      : "Clear Cache")) {
        if (m_confirm_clear) {
            if (nv2a_shader_cache_clear()) {
                m_cleared_time = ImGui::GetTime();
                nv2a_shader_precache_scan();
            }
            m_confirm_clear = false;
        } else {
            m_confirm_clear = true;
        }
    }
    ImGui::EndDisabled();
    PopButtonColors();

    ImGui::SameLine();
    PushGreenButtons();
    if (ImGui::Button("Close")) {
        is_open = false;
    }
    PopButtonColors();

    ImGui::End();
}
