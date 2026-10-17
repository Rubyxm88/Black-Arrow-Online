//
// xemu User Interface: Versions & Updates window (Help > Versions &
// Updates...) and the build badge at the right of the menu bar
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#pragma once

#include <string>
#include <vector>
#include <glib.h>

// A build on the releases page
struct ReleaseBuild {
    int         build;
    std::string date;    // YYYY-MM-DD
    std::string title;
    std::string notes;   // first line of the release notes
    std::string broken;  // why it must not be installed, or empty
    // Patch notes, one line each; "## " starts a heading, two spaces a
    // line that goes on from the one before
    std::vector<std::string> changes;
};

class VersionsWindow
{
public:
    enum State { IDLE, LOADING, LOADED, FAILED };

    bool is_open;

private:
    GMutex                    m_lock;  // guards what the thread writes
    State                     m_state;
    std::vector<ReleaseBuild> m_builds;  // newest first
    std::string               m_error;
    bool                      m_auto_checked;
    gint64                    m_loaded_time;  // monotonic, when loaded

    int         m_confirm;     // build to confirm installing, or 0
    bool        m_installing;  // closing to install
    std::string m_install_error;

public:
    VersionsWindow();
    void Open();
    void Draw();
    // Look once, a while after starting, whether a newer build is out
    void AutoCheck();
    // The newest installable build, or 0 when not known (yet)
    int NewestBuild();

private:
    void Load(const char *why);
    static gpointer LoadThread(gpointer self);
    void DrawConfirm();
    void Install(int build);
};

extern VersionsWindow versions_window;

// The installed build, "BA Online - Build N", at the right of the menu bar,
// with a dot when a newer build is out. Clicking it opens the Versions
// window.
void DrawBuildBadge();

// The build the player does not want to be offered (update-skip.txt, which
// the launcher's "Skip this build" also writes), or 0
int SkippedBuild();
