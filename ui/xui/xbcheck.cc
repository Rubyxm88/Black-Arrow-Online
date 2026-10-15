//
// xemu User Interface: XB Online connection check
//
// A small panel in the top left corner that shows the results of the
// connection check. It never takes focus, so the game keeps its controls.
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

XbCheckWindow xbcheck_window;

XbCheckWindow::XbCheckWindow()
{
    is_open = false;
    m_started = false;
    m_done_time = -1;
}

void XbCheckWindow::Start()
{
    xemu_xbcheck_start();
    m_started = true;
    m_done_time = -1;
    is_open = true;
}

void XbCheckWindow::Draw()
{
    if (!m_started) {
        if (g_config.general.show_welcome ||
            ImGui::GetTime() < FIRST_CHECK_DELAY) {
            return;
        }
        Start();
    }
    if (!is_open) {
        return;
    }

    XbCheckItem items[XBCHECK_MAX_ITEMS];
    bool running;
    int n = xemu_xbcheck_get(items, XBCHECK_MAX_ITEMS, &running);
    bool problem = false;
    for (int i = 0; i < n; i++) {
        problem |= items[i].state == XBCHECK_WARN ||
                   items[i].state == XBCHECK_FAIL;
    }

    double now = ImGui::GetTime();
    if (running) {
        m_done_time = -1;
    } else if (m_done_time < 0) {
        m_done_time = now;
    } else if (!problem && now - m_done_time > CLOSE_DELAY) {
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
            ImGui::Unindent(ImGui::CalcTextSize("  OK   ").x);
        }
    }

    if (!running) {
        ImGui::Separator();
        if (ImGui::Button("Check again")) {
            Start();
        }
        ImGui::SameLine();
        if (ImGui::Button("Close")) {
            is_open = false;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Results are also in system\\xemu.log");
    }
    ImGui::End();
}
