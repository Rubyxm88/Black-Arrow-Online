//
// xemu User Interface: XB Online connection check
//
// The connection check runs by itself at start. When something is wrong a
// panel in the top left corner shows what and how to fix it; when all is
// well a short "XB Online Connected" message appears instead. Neither takes
// focus, so the game keeps its controls.
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#include "common.hh"
#include "xbcheck.hh"
#include "viewport-manager.hh"
#include "../xemu-xbcheck.h"

/* Seconds after starting before the first check, so the network is up */
#define FIRST_CHECK_DELAY 3.0

/* Seconds a panel with nothing wrong stays on screen */
#define CLOSE_DELAY 10.0

/* Seconds the "XB Online Connected" message stays on screen */
#define TOAST_TIME 4.0

static const ImVec4 xbox_green(0.063f, 0.486f, 0.063f, 1.0f);
static const ImVec4 xbox_green_hover(0.10f, 0.62f, 0.10f, 1.0f);
static const ImVec4 xbox_green_active(0.05f, 0.40f, 0.05f, 1.0f);

/* Buttons in Xbox green, so they stand out on the dark panel */
static void PushGreenButtons()
{
    ImGui::PushStyleColor(ImGuiCol_Button, xbox_green);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, xbox_green_hover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, xbox_green_active);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
}

static void PopGreenButtons()
{
    ImGui::PopStyleColor(4);
}

XbCheckWindow xbcheck_window;

XbCheckWindow::XbCheckWindow()
{
    is_open = false;
    m_started = false;
    m_quiet = false;
    m_done_time = -1;
    m_toast_end = -1;
}

void XbCheckWindow::Start(bool show)
{
    xemu_xbcheck_start();
    m_started = true;
    m_quiet = !show;
    m_done_time = -1;
    m_toast_end = -1;
    is_open = show;
}

void XbCheckWindow::DrawToast()
{
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    float scale = g_viewport_mgr.m_scale;
    float pad = 10.0f * scale;
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + pad, vp->WorkPos.y + pad));
    ImGui::SetNextWindowBgAlpha(0.88f);
    if (ImGui::Begin("XB Online Connected", NULL,
                     ImGuiWindowFlags_NoDecoration |
                     ImGuiWindowFlags_NoFocusOnAppearing |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_AlwaysAutoResize)) {
        // A checkmark in a green circle, drawn: the font has no such glyph
        float size = ImGui::GetTextLineHeight();
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImVec2 c(p.x + size / 2, p.y + size / 2);
        ImDrawList *dl = ImGui::GetWindowDrawList();
        dl->AddCircleFilled(c, size / 2, ImGui::GetColorU32(xbox_green_hover));
        ImVec2 tick[3] = { ImVec2(c.x - size * 0.24f, c.y + size * 0.02f),
                           ImVec2(c.x - size * 0.06f, c.y + size * 0.20f),
                           ImVec2(c.x + size * 0.26f, c.y - size * 0.18f) };
        dl->AddPolyline(tick, 3, IM_COL32_WHITE, 0, 2.0f * scale);
        ImGui::Dummy(ImVec2(size, size));
        ImGui::SameLine();
        ImGui::Text("XB Online Connected");
    }
    ImGui::End();
}

void XbCheckWindow::Draw()
{
    if (!m_started) {
        if (g_config.general.show_welcome ||
            ImGui::GetTime() < FIRST_CHECK_DELAY) {
            return;
        }
        Start(false);
    }

    double now = ImGui::GetTime();
    if (m_toast_end > now) {
        DrawToast();
    }
    if (!is_open && !m_quiet) {
        return;
    }

    XbCheckItem items[XBCHECK_MAX_ITEMS];
    bool running;
    int n = xemu_xbcheck_get(items, XBCHECK_MAX_ITEMS, &running);
    bool problem = false, needs_action = false;
    for (int i = 0; i < n; i++) {
        problem |= items[i].state == XBCHECK_WARN ||
                   items[i].state == XBCHECK_FAIL;
        needs_action |= items[i].action != XBCHECK_ACTION_NONE;
    }

    if (m_quiet) {
        // Decide once the check has finished: panel or short message
        if (running) {
            return;
        }
        m_quiet = false;
        if (problem || needs_action) {
            is_open = true;
        } else {
            m_toast_end = now + TOAST_TIME;
            return;
        }
    }

    if (running) {
        m_done_time = -1;
    } else if (m_done_time < 0) {
        m_done_time = now;
    } else if (!problem && !needs_action && now - m_done_time > CLOSE_DELAY) {
        is_open = false;
        return;
    }

    const ImGuiViewport *vp = ImGui::GetMainViewport();
    float pad = 10.0f * g_viewport_mgr.m_scale;
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + pad, vp->WorkPos.y + pad));
    ImGui::SetNextWindowSize(ImVec2(520.0f * g_viewport_mgr.m_scale, 0));
    ImGui::SetNextWindowBgAlpha(0.88f);
    if (!ImGui::Begin("XB Online Check", NULL,
                      ImGuiWindowFlags_NoDecoration |
                      ImGuiWindowFlags_NoFocusOnAppearing |
                      ImGuiWindowFlags_NoNav |
                      ImGuiWindowFlags_NoSavedSettings |
                      ImGuiWindowFlags_NoMove)) {
        ImGui::End();
        return;
    }

    if (running) {
        ImGui::Text("XB Online check: checking...");
    } else if (problem) {
        ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.35f, 1.0f),
                           "XB Online check: fix the items below, then "
                           "restart Black Arrow Online");
    } else {
        ImGui::TextColored(ImVec4(0.45f, 0.9f, 0.45f, 1.0f),
                           "XB Online check: all good");
    }
    ImGui::Separator();

    for (int i = 0; i < n; i++) {
        // In XbCheckState order
        static const char *const marks[] = {
            "  ..  ", "  OK  ",
            " INFO ",  " WARN ",
            " FIX  ",
        };
        static const ImVec4 colors[] = {
            ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
            ImVec4(0.45f, 0.9f, 0.45f, 1.0f),
            ImVec4(0.55f, 0.75f, 1.0f, 1.0f),
            ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
            ImVec4(1.0f, 0.4f, 0.35f, 1.0f),
        };
        ImGui::TextColored(colors[items[i].state], "%s", marks[items[i].state]);
        ImGui::SameLine();
        ImGui::Text("%s", items[i].name);
        bool show_detail = items[i].state != XBCHECK_OK || !problem;
        if (show_detail) {
            ImGui::Indent(ImGui::CalcTextSize("  OK   ").x);
            ImGui::PushStyleColor(ImGuiCol_Text,
                                  ImVec4(0.8f, 0.8f, 0.8f, 1.0f));
            ImGui::TextWrapped("%s", items[i].detail);
            ImGui::PopStyleColor();
            if (items[i].action == XBCHECK_ACTION_OPEN_GAMES_FOLDER) {
                ImGui::PushID(i);
                PushGreenButtons();
                if (ImGui::Button("Open Games Folder")) {
                    xemu_xbcheck_open_games_folder();
                }
                PopGreenButtons();
                ImGui::PopID();
            }
            ImGui::Unindent(ImGui::CalcTextSize("  OK   ").x);
        }
    }

    if (!running) {
        ImGui::Separator();
        PushGreenButtons();
        if (ImGui::Button("Check Again")) {
            Start(true);
        }
        ImGui::SameLine();
        if (ImGui::Button("Close")) {
            is_open = false;
        }
        PopGreenButtons();
        ImGui::SameLine();
        ImGui::TextDisabled("Results are also in system\\xemu.log");
    }
    ImGui::End();
}
