//
// xemu User Interface: Shader Cache window
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#pragma once

#include "hw/xbox/nv2a/shader-precache.h"

class ShaderCacheWindow
{
public:
    bool is_open;

private:
    bool   m_auto_checked;   // looked once whether to precache by itself
    bool   m_scan_requested;
    bool   m_auto_running;   // a precache started by itself is running
    double m_toast_end;      // "Shaders ready" shown until then
    int    m_toast_built;
    bool   m_confirm_clear;  // Clear Cache was clicked once
    double m_cleared_time;   // when the cache was cleared, to say so

public:
    ShaderCacheWindow();
    void Open();
    void Draw();

private:
    void CheckAuto();
    void DrawToast(const ShaderPrecacheStatus &status);
};

extern ShaderCacheWindow shader_cache_window;
