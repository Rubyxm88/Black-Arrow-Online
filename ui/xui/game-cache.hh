//
// xemu User Interface: Game Cache window (Troubleshoot > Caches > Empty Game
// Cache...)
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#pragma once

#include <stdint.h>

class GameCacheWindow
{
public:
    bool is_open;

private:
    bool     m_has_disc;
    uint32_t m_title_id;
    char     m_name[40];
    int      m_has_cache;  // as xemu_gamecache_has()
    int      m_result;     // of the last Empty, or 2 for none yet

public:
    GameCacheWindow();
    void Open();
    void Draw();
};

extern GameCacheWindow game_cache_window;
