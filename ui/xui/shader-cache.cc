//
// xemu User Interface: Shader Cache window
//
// Shows what the shader cache holds, builds the shaders of the shipped list
// ahead of time (Precache Shaders) and empties the cache (Clear Cache). When
// there are shaders this PC has not built yet (first run, a driver update, a
// new resolution scale), they are built in the background by themselves,
// with a small notice, unless BA Online says not to.
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#include "common.hh"
#include "shader-cache.hh"
#include "notifications.hh"
#include "viewport-manager.hh"
#include "../xemu-settings.h"
#include "hw/xbox/nv2a/shader-precache.h"

/* Seconds after starting before looking whether to precache by itself */
#define AUTO_DELAY 12.0

/* How long "Shaders ready" stays up */
#define TOAST_TIME 4.0

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

ShaderCacheWindow shader_cache_window;

ShaderCacheWindow::ShaderCacheWindow()
{
    is_open = false;
    m_auto_checked = false;
    m_scan_requested = false;
    m_auto_running = false;
    m_toast_end = -1;
    m_toast_built = 0;
    m_confirm_clear = false;
    m_forget_learned = false;
    m_cleared_time = -1;
}

void ShaderCacheWindow::Open()
{
    nv2a_shader_precache_scan();
    m_confirm_clear = false;
    m_forget_learned = false;
    is_open = true;
}

// Once per run: build in the background whatever the list and this PC's own
// cache have that is not built for this driver and resolution scale yet
void ShaderCacheWindow::CheckAuto()
{
    if (m_auto_checked || !g_config.perf.shader_precache_auto ||
        ImGui::GetTime() < AUTO_DELAY) {
        return;
    }

    ShaderPrecacheStatus status;
    nv2a_shader_precache_get_status(&status);
    if (!status.supported) {
        /* Vulkan: nothing to do */
        m_auto_checked = true;
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
    m_auto_checked = true;
    if (status.list_missing > 0 && nv2a_shader_precache_start()) {
        m_auto_running = true;
    }
}

// Top left, under the XB Online one: progress, then "Shaders ready"
void ShaderCacheWindow::DrawToast(const ShaderPrecacheStatus &status)
{
    double now = ImGui::GetTime();
    if (m_auto_running && !status.running) {
        m_auto_running = false;
        m_toast_built = status.done;
        m_toast_end = now + TOAST_TIME;
    }
    if (!m_auto_running && m_toast_end <= now) {
        return;
    }

    float scale = g_viewport_mgr.m_scale;
    PopupStackPlace();
    ImGui::SetNextWindowBgAlpha(0.88f);
    if (ImGui::Begin("Shader Precache", NULL,
                     ImGuiWindowFlags_NoDecoration |
                     ImGuiWindowFlags_NoFocusOnAppearing |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_AlwaysAutoResize)) {
        if (m_auto_running) {
            int total = status.list_missing;
            int finished = status.done + status.failed;
            ImGui::Text("Building shaders in the background");
            char label[32];
            snprintf(label, sizeof(label), "%d / %d", finished, total);
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, xbox_green_hover);
            ImGui::ProgressBar(total ? (float)finished / total : 0.0f,
                               ImVec2(220.0f * scale, 0), label);
            ImGui::PopStyleColor();
        } else if (status.error[0]) {
            ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.35f, 1.0f), "%s",
                               status.error);
        } else {
            // A checkmark in a green circle, like the XB Online toast
            float size = ImGui::GetTextLineHeight();
            ImVec2 p = ImGui::GetCursorScreenPos();
            ImVec2 c(p.x + size / 2, p.y + size / 2);
            ImDrawList *dl = ImGui::GetWindowDrawList();
            dl->AddCircleFilled(c, size / 2,
                                ImGui::GetColorU32(xbox_green_hover));
            ImVec2 tick[3] = { ImVec2(c.x - size * 0.24f, c.y + size * 0.02f),
                               ImVec2(c.x - size * 0.06f, c.y + size * 0.20f),
                               ImVec2(c.x + size * 0.26f,
                                      c.y - size * 0.18f) };
            dl->AddPolyline(tick, 3, IM_COL32_WHITE, 0, 2.0f * scale);
            ImGui::Dummy(ImVec2(size, size));
            ImGui::SameLine();
            ImGui::Text("Shaders ready (%d built)", m_toast_built);
        }
        PopupStackTake();
    }
    ImGui::End();
}

void ShaderCacheWindow::Draw()
{
    CheckAuto();

    ShaderPrecacheStatus status;
    nv2a_shader_precache_get_status(&status);
    DrawToast(status);
    if (!is_open) {
        return;
    }

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
                           "BA Online > Renderer and restart.");
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
            ImGui::Text("Shaders this PC has used: %d", status.learned_count);
            if (status.skipped_count > 0) {
                ImGui::TextDisabled("%d skipped: they crashed the graphics "
                                    "driver while precaching.",
                                    status.skipped_count);
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
    if (m_confirm_clear) {
        ImGui::Separator();
        ImGui::TextWrapped("Clear Cache deletes the shaders built for this "
                           "graphics card. The list of shaders this PC has "
                           "used is kept, so they are built again in the "
                           "background.");
        ImGui::Checkbox("Also forget the shaders this PC has used",
                        &m_forget_learned);
        if (m_forget_learned) {
            ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.35f, 1.0f),
                               "Only if shaders keep causing problems: the "
                               "game stutters on each effect again until it "
                               "has seen it once.");
        }
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
            if (nv2a_shader_cache_clear(m_forget_learned)) {
                m_cleared_time = ImGui::GetTime();
                nv2a_shader_precache_scan();
            }
            m_confirm_clear = false;
            m_forget_learned = false;
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
