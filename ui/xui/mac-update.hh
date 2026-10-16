//
// xemu User Interface: Black Arrow Online updates on a Mac
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#pragma once

#include <string>

extern "C" {
#include "qemu/osdep.h"
#include "qemu/http.h"
}

class MacUpdateWindow
{
public:
    enum State {
        IDLE,
        CHECKING,
        UP_TO_DATE,
        AVAILABLE,
        CHECK_FAILED,
        DOWNLOADING,
        DOWNLOAD_FAILED,
        READY,         // unpacked; its setup.sh installs it once the app closes
        INSTALL_FAILED // the last update's setup.sh stopped
    };

    bool is_open;

private:
    GMutex      m_lock;       // guards what the threads write
    State       m_state;
    int         m_local_build;
    int         m_build;      // the newest build's number
    std::string m_notes;
    std::string m_sha256;
    std::string m_error;
    std::string m_setup;      // setup.sh of the unpacked update
    size_t      m_dlnow;
    size_t      m_dltotal;
    bool        m_cancel;

    bool        m_started;
    bool        m_auto_checked;
    bool        m_quiet;      // checked by itself: shown only for an update
    bool        m_later;      // "Later" was clicked
    bool        m_restarting;

public:
    MacUpdateWindow();
    // Help > Check for Updates
    void Open();
    void Draw();

private:
    void Check(bool quiet);
    void Download();
    void Restart();
    void ShowLastResult();
    void DrawContents(State state);
    static gpointer CheckThread(gpointer self);
    static gpointer DownloadThread(gpointer self);
    static int Progress(http_progress_cb_info *info);
};

extern MacUpdateWindow mac_update_window;
