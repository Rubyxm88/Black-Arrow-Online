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
#include "common.hh"
#include "actions.hh"
#include "misc.hh"
#include "xemu-hud.h"
#include "../xemu-snapshots.h"
#include "../xemu-notifications.h"
#include "../xemu-xbonline.h"
#include "snapshot-manager.hh"
#include "hw/xbox/xbox-audio-setting.h"
#include <filesystem>
#include <string>
#include <vector>
#if defined(__APPLE__)
#include <limits.h>
#include <mach-o/dyld.h>
#endif

void ActionEjectDisc(void)
{
    Error *err = NULL;
    xemu_eject_disc(&err);
    if (err) {
        xemu_queue_error_message(error_get_pretty(err));
        error_free(err);
    }
}

void ActionLoadDisc(void)
{
    static const SDL_DialogFileFilter filters[] = {
        { "Disc Image Files (*.iso, *.xiso)", "iso;xiso" },
        { "All Files", "*" }
    };
    const char *default_path = g_config.sys.files.dvd_path;
    if (!default_path || !default_path[0]) {
        default_path = g_config.general.games_dir;
    }
    ShowOpenFileDialog(filters, 2, default_path, [](const char *path) {
        ActionLoadDiscFile(path);
    });
}

void ActionLoadDiscFile(const char *file_path)
{
    Error *err = NULL;
    xemu_load_disc(file_path, &err);

    if (err) {
        xemu_queue_error_message(error_get_pretty(err));
        error_free(err);
    } else {
        const char *games_dir = g_config.general.games_dir;
        if (!games_dir || !games_dir[0]) {
            std::string dir = std::filesystem::path(file_path).parent_path().string();
            xemu_settings_set_string(&g_config.general.games_dir, dir.c_str());
        }
    }
}

void ActionPlayDiscFile(const char *file_path)
{
    Error *err = NULL;
    xemu_load_disc(file_path, &err);
    if (err) {
        xemu_queue_error_message(error_get_pretty(err));
        error_free(err);
        return;
    }
    // Restart so the new disc boots whatever was running before
    ActionReset();
}

void ActionXboxLiveSetup(void)
{
    // Installed as the console's dashboard, the XBL Dashboard starts with no
    // disc in, like on a real Xbox
    if (xemu_xbonline_main_dashboard() == XBONLINE_DASH_XBL) {
        ActionXboxDashboard();
        return;
    }
    // Otherwise it runs from its own disc image
    const char *disc = xemu_xbonline_setup_disc();
    if (disc) {
        ActionPlayDiscFile(disc);
    } else {
        ActionXboxDashboard();
    }
}

void ActionXboxDashboard(void)
{
    // With no disc in the drive the console starts the dashboard on its
    // hard disk
    ActionEjectDisc();
    ActionReset();
}

void ActionTogglePause(void)
{
    if (runstate_is_running()) {
        vm_stop(RUN_STATE_PAUSED);
    } else {
        vm_start();
    }
}

void ActionReset(void)
{
    qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
}

void ActionShutdown(void)
{
    qemu_system_shutdown_request(SHUTDOWN_CAUSE_HOST_UI);
}

void ActionScreenshot(void)
{
	g_screenshot_pending = true;
}

void ActionActivateBoundSnapshot(int slot, bool save)
{
    assert(slot < 4 && slot >= 0);
    const char *snapshot_name = *(g_snapshot_shortcut_index_key_map[slot]);
    if (!snapshot_name || !(snapshot_name[0])) {
        char *msg = g_strdup_printf("F%d is not bound to a snapshot", slot + 5);
        xemu_queue_notification(msg);
        g_free(msg);
        return;
    }

    Error *err = NULL;
    if (save) {
        xemu_snapshots_save(snapshot_name, &err);
    } else {
        ActionLoadSnapshotChecked(snapshot_name);
    }

    if (err) {
        xemu_queue_error_message(error_get_pretty(err));
        error_free(err);
    }
}

void ActionLoadSnapshotChecked(const char *name)
{
    g_snapshot_mgr.LoadSnapshotChecked(name);
}

static void SetXboxAudio(uint32_t flags)
{
    if (!xbox_audio_setting_set(flags)) {
        xemu_queue_notification("Could not change the Xbox's audio setting "
                                "(eeprom.bin).");
    }
}

void ActionSoundFast()
{
    g_config.audio.use_dsp = false;
}

void ActionSoundAccurate()
{
    g_config.audio.use_dsp = true;
    g_config.audio.use_dsp_jit = true;
    g_config.audio.channels = CONFIG_AUDIO_CHANNELS_STEREO;
    SetXboxAudio(XBOX_AUDIO_STEREO);
}

void ActionSoundChannels(int channels)
{
    g_config.audio.channels = channels;
    if (channels == CONFIG_AUDIO_CHANNELS_SURROUND) {
        // The game only sends 3D sound to the rear speakers with Dolby on,
        // and Accurate cannot run the Dolby encoder
        g_config.audio.use_dsp = false;
        SetXboxAudio(XBOX_AUDIO_STEREO | XBOX_AUDIO_AC3);
    } else {
        SetXboxAudio(XBOX_AUDIO_STEREO);
    }
}

bool SoundRestartNeeded(const char **name)
{
    uint32_t now, booted;
    if (!xbox_audio_setting_get(&now, &booted)) {
        return false;
    }
    if (name) {
        *name = xbox_audio_setting_name(now);
    }
    return SoundRestartReason() != SOUND_RESTART_NONE;
}

SoundRestart SoundRestartReason()
{
    uint32_t now, booted;
    if (!xbox_audio_setting_get(&now, &booted)) {
        return SOUND_RESTART_NONE;
    }
    if (g_config.audio.use_dsp && xbox_audio_setting_is_dolby(booted)) {
        return SOUND_RESTART_ACCURATE;
    }
    if (g_config.audio.channels == CONFIG_AUDIO_CHANNELS_SURROUND &&
        xbox_audio_setting_is_dolby(now) &&
        !xbox_audio_setting_is_dolby(booted)) {
        return SOUND_RESTART_SURROUND;
    }
    return SOUND_RESTART_NONE;
}

extern "C" {
// The app's command line (ui/xemu.c), to open it again the same way
extern int gArgc;
extern char **gArgv;
}

// What the app started with; set at the first call, from xemu_hud_init,
// before any setting can be changed
static bool start_hard_fpu, start_hard_sse, start_prelight;

std::string AppRestartSettings()
{
    static bool known;
    if (!known) {
        start_hard_fpu = g_config.perf.hard_fpu;
        start_hard_sse = g_config.perf.hard_sse;
        start_prelight = g_config.perf.prelight;
        known = true;
    }
    std::string names;
    auto add = [&names](bool changed, const char *name) {
        if (changed) {
            names += names.empty() ? "" : ", ";
            names += name;
        }
    };
#if defined(__x86_64__)
    // The others never use them
    add(g_config.perf.hard_fpu != start_hard_fpu, "Hard FPU");
    add(g_config.perf.hard_sse != start_hard_sse, "Hard SSE");
#endif
    add(g_config.perf.prelight != start_prelight, "Pre-light");
    return names;
}

#if defined(_WIN32)
// PowerShell waits for this process to end, then starts xemu.exe again with
// the same folder and arguments. Sent as -EncodedCommand so no path needs
// quoting for the command line.
static std::string PsQuote(const std::string &s)
{
    std::string out = "'";
    for (char c : s) {
        out += c;
        if (c == '\'') {
            out += '\'';
        }
    }
    return out + "'";
}

static bool StartAgainAfterExit()
{
    wchar_t exe_w[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(NULL, exe_w, G_N_ELEMENTS(exe_w));
    if (n == 0 || n >= G_N_ELEMENTS(exe_w)) {
        return false;
    }
    gchar *exe = g_utf16_to_utf8((const gunichar2 *)exe_w, n, NULL, NULL,
                                 NULL);
    gchar *cwd = g_get_current_dir();
    if (!exe) {
        g_free(cwd);
        return false;
    }
    std::string script = "$ErrorActionPreference='SilentlyContinue'; "
                         "Wait-Process -Id " +
                         std::to_string(GetCurrentProcessId()) +
                         " -Timeout 60; Start-Process -FilePath " +
                         PsQuote(exe) + " -WorkingDirectory " + PsQuote(cwd);
    if (gArgc > 1) {
        script += " -ArgumentList ";
        for (int i = 1; i < gArgc; i++) {
            script += (i > 1 ? "," : "") +
                      PsQuote(std::string("\"") + gArgv[i] + "\"");
        }
    }
    g_free(exe);
    g_free(cwd);

    glong len = 0;
    gunichar2 *u16 = g_utf8_to_utf16(script.c_str(), -1, NULL, &len, NULL);
    if (!u16) {
        return false;
    }
    gchar *b64 = g_base64_encode((const guchar *)u16, len * 2);
    g_free(u16);
    std::string cmd = std::string("powershell.exe -NoProfile -NonInteractive "
                                  "-WindowStyle Hidden -EncodedCommand ") +
                      b64;
    g_free(b64);
    gunichar2 *cmd_w = g_utf8_to_utf16(cmd.c_str(), -1, NULL, NULL, NULL);
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    BOOL ok = CreateProcessW(NULL, (wchar_t *)cmd_w, NULL, NULL, FALSE,
                             CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    g_free(cmd_w);
    if (!ok) {
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}
#else
// Out of the app's session, so closing the app does not end it
static void RestartDetach(gpointer)
{
    setsid();
}

// bash waits (up to a minute) for this process to end, then runs the rest
// of its arguments
static const char *const restart_script =
    "pid=\"$1\"; shift; i=0; "
    "while kill -0 \"$pid\" 2>/dev/null && [ $i -lt 300 ]; do "
    "sleep 0.2; i=$((i + 1)); done; exec \"$@\"";

static bool StartAgainAfterExit()
{
    std::vector<std::string> cmd;
#if defined(__APPLE__)
    char path[PATH_MAX];
    uint32_t size = sizeof(path);
    char *real = _NSGetExecutablePath(path, &size) == 0
                     ? realpath(path, NULL) : NULL;
    if (!real) {
        return false;
    }
    std::string exe = real;
    free(real);
    size_t app = exe.find(".app/Contents/MacOS/");
    if (app != std::string::npos) {
        // The bundle, so the Dock and the app's settings come with it
        cmd = { "open", "-n", exe.substr(0, app + 4) };
        if (gArgc > 1) {
            cmd.push_back("--args");
        }
    } else {
        cmd = { exe };
    }
#else
    // An installed Linux copy runs from app/ next to system/ (AppRun sets
    // up its libraries); otherwise this program itself
    gchar *system = g_strdup(xemu_settings_get_base_path());
    size_t n = strlen(system);
    while (n > 1 && system[n - 1] == '/') {
        system[--n] = 0;
    }
    gchar *data = g_path_get_dirname(system);
    gchar *apprun = g_build_filename(data, "app", "AppRun", NULL);
    gchar *self = g_file_read_link("/proc/self/exe", NULL);
    if (g_file_test(apprun, G_FILE_TEST_IS_EXECUTABLE)) {
        cmd = { apprun };
    } else if (self) {
        cmd = { self };
    }
    g_free(self);
    g_free(apprun);
    g_free(data);
    g_free(system);
    if (cmd.empty()) {
        return false;
    }
#endif
    for (int i = 1; i < gArgc; i++) {
        cmd.push_back(gArgv[i]);
    }

    std::string pid = std::to_string((int)getpid());
    std::vector<const gchar *> argv = { "/bin/bash", "-c", restart_script,
                                        "bao-restart", pid.c_str() };
    for (auto &s : cmd) {
        argv.push_back(s.c_str());
    }
    argv.push_back(NULL);
    GError *err = NULL;
    if (!g_spawn_async(NULL, (gchar **)argv.data(), NULL,
                       (GSpawnFlags)(G_SPAWN_STDOUT_TO_DEV_NULL |
                                     G_SPAWN_STDERR_TO_DEV_NULL),
                       RestartDetach, NULL, NULL, &err)) {
        g_clear_error(&err);
        return false;
    }
    return true;
}
#endif

bool ActionRestartApp()
{
    xemu_settings_save();
    if (!StartAgainAfterExit()) {
        xemu_queue_error_message("Could not open Black Arrow Online again. "
                                 "Close it and open it yourself.");
        return false;
    }
    fprintf(stderr, "restart: closing to apply settings\n");
    ActionShutdown();
    return true;
}
