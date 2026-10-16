//
// xemu User Interface: Black Arrow Online guide (Help > Black Arrow Online
// Guide...)
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#pragma once

class GuideWindow
{
public:
    bool is_open;

private:
    int m_page;

public:
    GuideWindow();
    void Open(int page = 0);
    void Draw();
};

extern GuideWindow guide_window;
