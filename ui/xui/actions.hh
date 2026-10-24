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
#pragma once

void ActionEjectDisc();
void ActionLoadDisc();
void ActionLoadDiscFile(const char *file_path);
void ActionPlayDiscFile(const char *file_path);
void ActionXboxLiveSetup();
void ActionXboxDashboard();
void ActionTogglePause();
void ActionReset();
void ActionShutdown();
void ActionScreenshot();
void ActionActivateBoundSnapshot(int slot, bool save);
void ActionLoadSnapshotChecked(const char *name);

// Sound: pick Fast or Accurate and the speaker layout. Each also sets the
// Xbox's own audio setting (Accurate needs Stereo, 5.1 needs Dolby Digital),
// which the game uses from its next start.
void ActionSoundFast();
void ActionSoundAccurate();
void ActionSoundChannels(int channels);
// What the chosen sound waits for until the game restarts, given the Xbox
// audio setting the game started with. A changed setting alone does not
// count: Fast in stereo plays the same on any setting, so it is just used
// from the next start.
enum SoundRestart {
    SOUND_RESTART_NONE,
    SOUND_RESTART_ACCURATE,  // Accurate, but the game started on Dolby
    SOUND_RESTART_SURROUND,  // 5.1, but the game started without Dolby
};
SoundRestart SoundRestartReason();
// Whether the chosen sound needs a game restart (SoundRestartReason).
// name gets the Xbox audio setting now ("Stereo", "Dolby Digital", ...)
bool SoundRestartNeeded(const char **name);
