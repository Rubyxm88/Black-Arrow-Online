//
// xemu User Interface: one-time XBL Dashboard install
//
// The package ships the XBL Dashboard installer disc. When the hard disk
// still boots another dashboard, xemu boots that disc once and does the
// install for the player:
//
//   1. the installer starts (its only screen says "A: install")
//   2. xemu presses A; the hard disk then starts taking writes, which is how
//      xemu knows the install began (A is pressed again if it did not)
//   3. the installer copies its files, makes a backup of the old dashboard,
//      and restarts the console when it is done
//   4. xemu takes the disc out, saves the hard disk, checks that the console
//      now boots the XBL Dashboard, and restarts into it
//
// A panel covers the screen the whole time, with a progress bar, so the
// player sees one setup step instead of the installer's own screens. If the
// install does not finish, the old dashboard stays (the installer only swaps
// it at the end) and Play > Install XBL Dashboard tries again.
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#include "common.hh"
#include "dash-install.hh"
#include "actions.hh"
#include "viewport-manager.hh"
#include "font-manager.hh"
#include "gl-helpers.hh"
#include "xbcheck.hh"
#include "../xemu-xbcheck.h"
#include "../xemu-dashinstall.h"
#include "../xemu-xbonline.h"
#include "../xemu-notifications.h"
#include "../xemu-input.h"
#include "../xemu-settings.h"
#include <algorithm>
#include <math.h>

/* Seconds after the installer disc starts booting before the first A */
#define FIRST_PRESS 22.0
/* How long A is held: the Xbox reads the controller once per frame */
#define PRESS_TIME 0.4
/* Seconds after a press to look for the install's disk writes */
#define START_CHECK 7.0
/* Presses before giving up */
#define MAX_PRESSES 8
/* Bytes the install writes; only for the progress bar */
#define EXPECTED_WRITE (24.0 * 1024 * 1024)
/* Writes that show the install has begun */
#define STARTED_WRITE (2 * 1024 * 1024)
/* Seconds the install may take before xemu gives up on it */
#define INSTALL_TIMEOUT 240.0
/* Seconds the panel stays while the XBL Dashboard boots */
#define DASH_BOOT_TIME 14.0
/* Automatic installs before xemu stops trying by itself */
#define MAX_AUTO_TRIES 2

enum Stage {
    STAGE_IDLE,
    STAGE_BOOTING,     // installer disc booting
    STAGE_PRESSING,    // A held down
    STAGE_WAIT_START,  // A let go; did the disk start taking writes?
    STAGE_INSTALLING,  // installer copying; waiting for its restart
    STAGE_RESTARTING,  // disc out, restarting into the XBL Dashboard
};

static Stage stage = STAGE_IDLE;
static double stage_time;      // when the current stage began
static double begin_time;      // when the install began, for the panel
static bool timed;             // stage_time set from the UI clock yet
static int presses;
static uint64_t press_written; // disk writes when A was last pressed
static uint64_t install_written;
static int resets_at_install;  // console restarts seen when it began

static double Now()
{
    return g_get_monotonic_time() / 1e6;
}

static void SetStage(Stage s)
{
    stage = s;
    stage_time = Now();
}

static uint64_t HddWritten()
{
    return xemu_dashinstall_hdd_written();
}

/*
 * A only reaches the Xbox through a controller in a port. With none (a pad
 * set in the settings but not plugged in), put the keyboard in port 1 for
 * this run.
 */
static void EnsureController()
{
    for (int i = 0; i < 4; i++) {
        if (xemu_input_get_bound(i)) {
            return;
        }
    }
    ControllerState *iter;
    QTAILQ_FOREACH(iter, &available_controllers, entry) {
        if (iter->type == INPUT_DEVICE_SDL_KEYBOARD) {
            xemu_input_bind(0, iter, 0);
            fprintf(stderr, "dash install: no controller, keyboard in port 1\n");
            return;
        }
    }
}

static void Fail(const char *why)
{
    __atomic_store_n(&xemu_input_forced_buttons, (uint16_t)(0), __ATOMIC_RELAXED);
    fprintf(stderr, "dash install: failed: %s\n", why);
    stage = STAGE_IDLE;
    // The installer swaps the dashboard last, so the old one still boots
    ActionXboxDashboard();
    char *msg = g_strdup_printf(
        "XBL Dashboard setup did not finish (%s). Your dashboard is "
        "unchanged; Play > Install XBL Dashboard tries again.", why);
    xemu_queue_error_message(msg);
    g_free(msg);
}

/* The installer has restarted the console: finish */
static void Finish()
{
    __atomic_store_n(&xemu_input_forced_buttons, (uint16_t)(0), __ATOMIC_RELAXED);
    ActionEjectDisc();

    // Write the hard disk's tables out now, so the new files are whole on
    // disk even if xemu is closed hard later, and so the check below reads
    // what the console wrote
    xemu_dashinstall_hdd_flush();
    int dash = xemu_xbonline_hdd_main_dashboard(g_config.sys.files.hdd_path);
    fprintf(stderr, "dash install: installer done after %.0f s, %.1f MB "
            "written, dashboard now %d\n", Now() - begin_time,
            (HddWritten() - install_written) / 1048576.0, dash);
    if (dash != XBONLINE_DASH_XBL) {
        stage = STAGE_IDLE;
        ActionReset();
        xemu_queue_error_message(
            "XBL Dashboard setup did not finish: the console still boots its "
            "old dashboard. Play > Install XBL Dashboard tries again.");
        return;
    }
    xemu_xbonline_set_main_dashboard(XBONLINE_DASH_XBL);
    g_config.general.xb_online_setup_offered = true;
    xemu_settings_save();
    ActionReset();
    SetStage(STAGE_RESTARTING);
}

static void Step()
{
    double t = Now() - stage_time;

    switch (stage) {
    case STAGE_BOOTING:
        if (t >= (presses ? START_CHECK : FIRST_PRESS)) {
            EnsureController();
            press_written = HddWritten();
            presses++;
            fprintf(stderr, "dash install: pressing A (%d)\n", presses);
            __atomic_store_n(&xemu_input_forced_buttons, (uint16_t)(CONTROLLER_BUTTON_A), __ATOMIC_RELAXED);
            SetStage(STAGE_PRESSING);
        }
        break;
    case STAGE_PRESSING:
        if (t >= PRESS_TIME) {
            __atomic_store_n(&xemu_input_forced_buttons, (uint16_t)(0), __ATOMIC_RELAXED);
            SetStage(STAGE_WAIT_START);
        }
        break;
    case STAGE_WAIT_START:
        if (HddWritten() - press_written >= STARTED_WRITE) {
            fprintf(stderr, "dash install: install started\n");
            install_written = press_written;
            resets_at_install = xemu_dashinstall_resets();
            SetStage(STAGE_INSTALLING);
        } else if (t >= START_CHECK) {
            if (presses >= MAX_PRESSES) {
                Fail("the installer did not start");
            } else {
                // Not ready for A yet: press again right away
                stage = STAGE_BOOTING;
                stage_time = Now() - START_CHECK;
            }
        }
        break;
    case STAGE_INSTALLING:
        if (xemu_dashinstall_resets() != resets_at_install) {
            Finish();
        } else if (t >= INSTALL_TIMEOUT) {
            Fail("the installer took too long");
        }
        break;
    case STAGE_RESTARTING:
        if (t >= DASH_BOOT_TIME) {
            stage = STAGE_IDLE;
            xemu_queue_notification("XBL Dashboard is set up. Pick your game "
                                    "in the Play menu.");
        }
        break;
    default:
        break;
    }
}

/* 0..1 for the bar, and what it is doing */
static float Progress(const char **what)
{
    double t = Now() - stage_time;

    switch (stage) {
    case STAGE_BOOTING:
    case STAGE_PRESSING:
    case STAGE_WAIT_START: {
        *what = "Starting the XBL Dashboard installer";
        double since = Now() - begin_time;
        return (float)std::min(0.15, 0.15 * since / (FIRST_PRESS + START_CHECK));
    }
    case STAGE_INSTALLING: {
        *what = "Copying XBL Dashboard to the console";
        double done = (HddWritten() - install_written) / EXPECTED_WRITE;
        return 0.15f + 0.75f * (float)std::min(done, 1.0);
    }
    case STAGE_RESTARTING:
        *what = "Restarting into XBL Dashboard";
        return 0.90f + 0.10f * (float)std::min(t / DASH_BOOT_TIME, 1.0);
    default:
        *what = "";
        return 1.0f;
    }
}

static const ImVec4 green(0.30f, 0.78f, 0.34f, 1.0f);
static const ImVec4 dim(0.62f, 0.65f, 0.66f, 1.0f);
static const ImVec4 red(1.0f, 0.42f, 0.36f, 1.0f);
static const ImVec4 amber(1.0f, 0.78f, 0.30f, 1.0f);

enum StepState { STEP_WAITING, STEP_RUNNING, STEP_DONE };

/* A step: a ring (waiting), a pulsing dot (running) or a tick (done) */
static void StepRow(const char *label, StepState state)
{
    float h = ImGui::GetTextLineHeight();
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImVec2 c(p.x + h / 2, p.y + h / 2);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    float scale = g_viewport_mgr.m_scale;

    if (state == STEP_DONE) {
        dl->AddCircleFilled(c, h * 0.45f, ImGui::GetColorU32(green));
        ImVec2 tick[3] = { ImVec2(c.x - h * 0.22f, c.y + h * 0.02f),
                           ImVec2(c.x - h * 0.06f, c.y + h * 0.18f),
                           ImVec2(c.x + h * 0.24f, c.y - h * 0.16f) };
        dl->AddPolyline(tick, 3, IM_COL32_WHITE, 0, 2.0f * scale);
    } else if (state == STEP_RUNNING) {
        float pulse = 0.6f + 0.4f * sinf((float)ImGui::GetTime() * 5.0f);
        dl->AddCircle(c, h * 0.45f, ImGui::GetColorU32(green), 0,
                      1.5f * scale);
        dl->AddCircleFilled(c, h * 0.22f * pulse + h * 0.08f,
                            ImGui::GetColorU32(green));
    } else {
        dl->AddCircle(c, h * 0.45f, ImGui::GetColorU32(dim), 0,
                      1.5f * scale);
    }
    ImGui::Dummy(ImVec2(h, h));
    ImGui::SameLine(0, 10.0f * scale);
    if (state == STEP_WAITING) {
        ImGui::TextColored(dim, "%s", label);
    } else {
        ImGui::Text("%s", label);
    }
}

/* The XB Online check, as short rows under its step */
static void DrawChecks(bool *has_fix)
{
    XbCheckItem items[XBCHECK_MAX_ITEMS];
    bool running = true;
    int n = xemu_xbcheck_get(items, XBCHECK_MAX_ITEMS, &running);
    StepState state = n == 0 ? STEP_WAITING :
                      running ? STEP_RUNNING : STEP_DONE;

    *has_fix = false;
    StepRow("Check the connection to XB Online", state);
    if (n == 0) {
        return;
    }
    float indent = ImGui::GetTextLineHeight() + 10.0f * g_viewport_mgr.m_scale;
    ImGui::Indent(indent);
    ImGui::PushFont(g_font_mgr.m_default_font);
    // Two columns of short results
    if (ImGui::BeginTable("##checks", 2)) {
        for (int i = 0; i < n; i++) {
            // In XbCheckState order
            static const char *const marks[] = { "...", "OK", "OK", "!",
                                                 "FIX" };
            const ImVec4 colors[] = { dim, green, green, amber, red };
            ImGui::TableNextColumn();
            ImGui::TextColored(colors[items[i].state], "%s",
                               marks[items[i].state]);
            ImGui::SameLine(ImGui::CalcTextSize("FIX  ").x);
            ImGui::TextColored(dim, "%s", items[i].name);
            *has_fix |= items[i].state == XBCHECK_FAIL ||
                        items[i].state == XBCHECK_WARN;
        }
        ImGui::EndTable();
    }
    ImGui::PopFont();
    ImGui::Unindent(indent);
}

void DashInstallDraw()
{
    if (stage == STAGE_IDLE) {
        return;
    }
    if (!timed) {
        // The machine starts with the UI; time from the first frame
        timed = true;
        begin_time = Now();
        stage_time = begin_time;
        xemu_dashinstall_resets();  // start counting restarts
        xemu_settings_save();       // the try counts even if xemu crashes
        // The connection check shows here, not in its own panel
        xbcheck_window.hidden = true;
    }
    Step();
    if (stage == STAGE_IDLE) {
        // Its results stay one click away: the one-line summary
        xbcheck_window.hidden = false;
        xbcheck_window.ShowShort();
        return;
    }

    const char *what;
    float progress = Progress(&what);
    float scale = g_viewport_mgr.m_scale;
    const ImGuiViewport *vp = ImGui::GetMainViewport();

    // Cover the installer's own screens
    ImGui::GetBackgroundDrawList()->AddRectFilled(
        vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y),
        IM_COL32(12, 14, 12, 255));

    float width = 600.0f * scale;
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(width, 0), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                        ImVec2(28.0f * scale, 24.0f * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f * scale);
    if (ImGui::Begin("##dash-install", NULL,
                     ImGuiWindowFlags_NoDecoration |
                         ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoFocusOnAppearing |
                         ImGuiWindowFlags_NoNav)) {
        float inner = ImGui::GetContentRegionAvail().x;

        // Logo
        if (g_bao_logo_tex && g_bao_logo_w > 0) {
            float lw = std::min(inner, 400.0f * scale);
            float lh = lw * g_bao_logo_h / g_bao_logo_w;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (inner - lw) / 2);
            ImGui::Image((ImTextureID)(intptr_t)g_bao_logo_tex,
                         ImVec2(lw, lh));
            ImGui::Dummy(ImVec2(0, 14.0f * scale));
        }

        // Title and what it means
        ImGui::PushFont(g_font_mgr.m_menu_font);
        ImGui::Text("Setting up your console");
        ImGui::PopFont();
        ImGui::PushFont(g_font_mgr.m_default_font);
        ImGui::TextColored(dim, "One time only, about two minutes. Please "
                                "don't close xemu.");
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(0, 12.0f * scale));
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0, 10.0f * scale));

        // Steps
        ImGui::PushFont(g_font_mgr.m_menu_font_small);
        bool has_fix;
        DrawChecks(&has_fix);
        ImGui::Dummy(ImVec2(0, 6.0f * scale));
        bool starting = stage == STAGE_BOOTING || stage == STAGE_PRESSING ||
                        stage == STAGE_WAIT_START;
        StepRow("Start the installer", starting ? STEP_RUNNING : STEP_DONE);
        ImGui::Dummy(ImVec2(0, 6.0f * scale));
        StepRow("Install XBL Dashboard",
                stage == STAGE_INSTALLING ? STEP_RUNNING :
                starting                  ? STEP_WAITING : STEP_DONE);
        ImGui::Dummy(ImVec2(0, 6.0f * scale));
        StepRow("Start XBL Dashboard",
                stage == STAGE_RESTARTING ? STEP_RUNNING : STEP_WAITING);
        ImGui::PopFont();

        // Overall progress
        ImGui::Dummy(ImVec2(0, 16.0f * scale));
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram,
                              ImVec4(0.20f, 0.62f, 0.24f, 1.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f * scale);
        ImGui::ProgressBar(progress, ImVec2(-1, 10.0f * scale), "");
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        ImGui::PushFont(g_font_mgr.m_default_font);
        ImGui::TextColored(dim, "%s", what);
        char pct[8];
        snprintf(pct, sizeof(pct), "%d%%", (int)(progress * 100 + 0.5f));
        ImGui::SameLine(inner - ImGui::CalcTextSize(pct).x +
                        ImGui::GetStyle().WindowPadding.x);
        ImGui::Text("%s", pct);

        ImGui::Dummy(ImVec2(0, 10.0f * scale));
        ImGui::PushStyleColor(ImGuiCol_Text, dim);
        ImGui::TextWrapped("XBL Dashboard becomes your console's dashboard; "
                           "the original one is kept as a backup.");
        ImGui::PopStyleColor();
        if (has_fix) {
            ImGui::TextColored(amber, "Items marked FIX or ! are explained "
                                      "once setup is done.");
        }
        ImGui::PopFont();
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
}

extern "C" {

bool xemu_dashinstall_wanted(void)
{
    return xemu_xbonline_install_disc() &&
           xemu_xbonline_main_dashboard() == XBONLINE_DASH_STOCK &&
           g_config.general.xbl_dash_install_tries < MAX_AUTO_TRIES &&
           !getenv("XEMU_NO_DASH_INSTALL");
}

void xemu_dashinstall_begin(void)
{
    // Counting restarts and saving the settings wait for the first frame:
    // this runs before the machine is set up
    g_config.general.xbl_dash_install_tries++;
    presses = 0;
    timed = false;
    stage = STAGE_BOOTING;
    fprintf(stderr, "dash install: booting %s (try %d)\n",
            XEMU_XBONLINE_INSTALL_DISC,
            g_config.general.xbl_dash_install_tries);
}

void xemu_dashinstall_start_now(void)
{
    const char *disc = xemu_xbonline_install_disc();
    if (!disc || stage != STAGE_IDLE) {
        return;
    }
    ActionPlayDiscFile(disc);
    xemu_dashinstall_begin();
}

bool xemu_dashinstall_active(void)
{
    return stage != STAGE_IDLE;
}

}
