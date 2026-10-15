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
    double m_done_time;

public:
    XbCheckWindow();
    void Start();
    void Draw();
};

extern XbCheckWindow xbcheck_window;
