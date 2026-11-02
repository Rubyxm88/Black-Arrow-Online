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
#include "../xemu-dashinstall.h"
#include "../xemu-xbonline.h"
#include "../xemu-notifications.h"
#include "../xemu-input.h"
#include "../xemu-settings.h"

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
        *what = "Starting the installer...";
        double since = Now() - begin_time;
        return (float)MIN(0.15, 0.15 * since / (FIRST_PRESS + START_CHECK));
    }
    case STAGE_INSTALLING: {
        *what = "Installing XBL Dashboard...";
        double done = (HddWritten() - install_written) / EXPECTED_WRITE;
        return 0.15f + 0.75f * (float)MIN(done, 1.0);
    }
    case STAGE_RESTARTING:
        *what = "Starting XBL Dashboard...";
        return 0.90f + 0.10f * (float)MIN(t / DASH_BOOT_TIME, 1.0);
    default:
        *what = "";
        return 1.0f;
    }
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
    }
    Step();
    if (stage == STAGE_IDLE) {
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

    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(520.0f * scale, 0), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(1.0f);
    if (ImGui::Begin("##dash-install", NULL,
                     ImGuiWindowFlags_NoDecoration |
                         ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoFocusOnAppearing |
                         ImGuiWindowFlags_NoNav)) {
        ImGui::Dummy(ImVec2(0, 6.0f * scale));
        ImGui::Text("Setting up XBL Dashboard");
        ImGui::Dummy(ImVec2(0, 4.0f * scale));
        ImGui::TextWrapped("This happens one time only. XBL Dashboard "
                           "becomes your console's dashboard; the old one is "
                           "kept as a backup. Please don't close xemu.");
        ImGui::Dummy(ImVec2(0, 10.0f * scale));
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram,
                              ImVec4(0.063f, 0.486f, 0.063f, 1.0f));
        ImGui::ProgressBar(progress, ImVec2(-1, 0), "");
        ImGui::PopStyleColor();
        ImGui::TextDisabled("%s  %d%%", what, (int)(progress * 100 + 0.5f));
        ImGui::Dummy(ImVec2(0, 6.0f * scale));
    }
    ImGui::End();
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
    xemu_dashinstall_resets();  // start counting restarts
    g_config.general.xbl_dash_install_tries++;
    xemu_settings_save();
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
