//
// xemu User Interface: XB Online connection check
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#pragma once

class XbCheckWindow
{
public:
    bool is_open;

private:
    bool   m_started;
    bool   m_quiet;      // started by itself: show the panel only on problems
    double m_done_time;
    double m_toast_end;  // "XB Online Connected" shown until then

public:
    XbCheckWindow();
    // show: open the panel right away (the player asked for the check)
    void Start(bool show);
    void Draw();

private:
    void DrawToast();
};

extern XbCheckWindow xbcheck_window;
