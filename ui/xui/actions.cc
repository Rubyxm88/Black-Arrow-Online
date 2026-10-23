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
    // The XBL Dashboard, where the Xbox Live account is managed, runs from
    // its own disc image
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
        *name = (now & XBOX_AUDIO_DTS)                         ? "DTS"
                : (now & XBOX_AUDIO_AC3)                       ? "Dolby Digital"
                : (now & 0xFF) == XBOX_AUDIO_DOLBY_SURROUND    ? "Dolby Surround"
                : (now & 0xFF) == 1                            ? "Mono"
                                                               : "Stereo";
    }
    return ((now ^ booted) & XBOX_AUDIO_SETTING_MASK) != 0;
}
